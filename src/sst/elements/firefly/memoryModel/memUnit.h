// Copyright 2009-2018 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2018, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

    class MemUnit : public Unit {
        enum Op { Read, Write };
      public:
        MemUnit( SimpleMemoryModel& model, Output& dbg, int id, int readLat_ns, int writeLat_ns, int numSlots ) :
            Unit( model, dbg ), m_pending(0), m_readLat_ns(readLat_ns), m_writeLat_ns(writeLat_ns), m_numSlots(numSlots)
        {
            m_prefix = "@t:" + std::to_string(id) + ":SimpleMemoryModel::MemUnit::@p():@l ";
			m_latency = model.registerStatistic<uint64_t>("mem_blocked_time");

        }

        bool store( UnitBase* src, MemReq* req ) {
            m_dbg.verbosePrefix(prefix(),CALL_INFO,1,MEM_MASK,"addr=%#" PRIx64 " length=%lu\n",req->addr, req->length);
            return work( m_writeLat_ns, Write, req, src, m_model.getCurrentSimTimeNano() );
        }

        bool load( UnitBase* src, MemReq* req, Callback* callback ) {
            m_dbg.verbosePrefix(prefix(),CALL_INFO,1,MEM_MASK,"addr=%#" PRIx64 " length=%lu\n",req->addr,req->length);
            return work( m_readLat_ns, Read, req, src, m_model.getCurrentSimTimeNano(), callback );
        }

      private:

        struct Entry {

            void init( SimTime_t _delay, Op _op, MemReq* _memReq, UnitBase* _src, Callback* _callback, SimTime_t _qTime ) { 
                delay = _delay;
				op = _op;
				memReq = _memReq;
				src = _src;
				callback = _callback;
				qTime = _qTime;
            }
            SimTime_t delay;
            Op op;
			MemReq* memReq;
            UnitBase* src;
			Callback* callback;
            SimTime_t qTime;
        };

		struct LambdaData {
			SimTime_t issueTime;
			SimTime_t qTime;
			Callback* callback;
			MemReq*   req;	
			Op	      op;
		};

		ThingHeap< LambdaData> m_ldHeap;
		ThingHeap< Entry> m_entryHeap;

        bool work( SimTime_t delay, Op op, MemReq* req,  UnitBase* src, SimTime_t qTime, Callback* callback = NULL ) {

            if ( m_pending == m_numSlots ) {

				m_dbg.verbosePrefix(prefix(),CALL_INFO,1,MEM_MASK,"blocking src\n");
				Entry* entry = m_entryHeap.alloc();
				entry->init( delay, op, req, src, callback, m_model.getCurrentSimTimeNano() ); 
                m_blocked.push( entry );
				m_blockedTime = m_model.getCurrentSimTimeNano();
                return true;
            }

            ++m_pending;
		
			Callback* cb = m_model.cbAlloc();

			LambdaData* ld = m_ldHeap.alloc();
            ld->issueTime  = m_model.getCurrentSimTimeNano();
			ld->op = op;
			ld->callback = callback;
			ld->qTime = qTime;
			ld->req = req;

            *cb = [this,ld]()
                {
                    --m_pending;

                    SimTime_t latency = m_model.getCurrentSimTimeNano() - ld->issueTime;

                    m_dbg.verbosePrefix(prefix(),CALL_INFO,1,MEM_MASK,"%s complete latency=%" PRIu64 " qLatency=%" PRIu64 " addr=%#" PRIx64 " length=%lu\n",
                                                        ld->op == Read ? "Read":"Write" ,latency, ld->issueTime-ld->qTime, ld->req->addr, ld->req->length);

                    if ( ld->callback ) {
                        m_model.schedCallback( 0, ld->callback);
                    }

					m_model.memReqFree( ld->req );

                    if ( ! m_blocked.empty() ) {
		
						SimTime_t latency = m_model.getCurrentSimTimeNano() - m_blockedTime;
						if ( latency ) {
							m_latency->addData( latency );
						}
                        Entry* entry = m_blocked.front( );
                        m_blocked.pop();

                        work( entry->delay, entry->op, entry->memReq, entry->src, entry->qTime, entry->callback );
                		m_model.schedResume( 0, entry->src, (UnitBase*) ( entry->op == Read ? "R" : "W" ) );
						m_entryHeap.free(entry);
                    }
					m_ldHeap.free(ld);
                };

            m_model.schedCallback( delay, cb ); 

			return false;
        }

		SimTime_t m_blockedTime;
        Statistic<uint64_t>* m_latency;
        std::queue< Entry* > m_blocked;
        int m_pending;
        int m_numSlots;
        int m_readLat_ns;
        int m_writeLat_ns;
    };
