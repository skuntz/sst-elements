// Copyright 2009-2014 Sandia Corporation. Under the terms
// of Contract DE-AC04-94AL85000 with Sandia Corporation, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2014, Sandia Corporation
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.


#ifndef _H_EMBER_3D_AMR
#define _H_EMBER_3D_AMR

#include "mpi/embermpigen.h"

namespace SST {
namespace Ember {

class Ember3DAMRBlock {
        public:
                Ember3DAMRBlock(
                        const uint32_t id,
                        const uint32_t refLevel,
                        const int32_t ref_x_up,
                        const int32_t ref_x_down,
                        const int32_t ref_y_up,
                        const int32_t ref_y_down,
                        const int32_t ref_z_up,
                        const int32_t ref_z_down ) :

                        blockID(id),
                        refinementLevel(refLevel),
                        refine_x_up(ref_x_up),
                        refine_x_down(ref_x_down),
                        refine_y_up(ref_y_up),
                        refine_y_down(ref_y_down),
                        refine_z_up(ref_z_up),
                        refine_z_down(ref_z_down) {

                        commXUp   = (int32_t*) malloc(sizeof(int32_t) * 4);
                        commXDown = (int32_t*) malloc(sizeof(int32_t) * 4);
                        commYUp   = (int32_t*) malloc(sizeof(int32_t) * 4);
                        commYDown = (int32_t*) malloc(sizeof(int32_t) * 4);
                        commZUp   = (int32_t*) malloc(sizeof(int32_t) * 4);
                        commZDown = (int32_t*) malloc(sizeof(int32_t) * 4);

                }

                ~Ember3DAMRBlock() {
                        free(commXUp);
                        free(commXDown);
                        free(commYUp);
                        free(commYDown);
                        free(commZUp);
                        free(commZDown);
                }

                uint32_t getRefinementLevel() const {
                        return refinementLevel;
                }

                uint32_t getBlockID() const {
                        return blockID;
                }

		               int32_t getRefineXUp() const {
                        return refine_x_up;
                }

                int32_t getRefineXDown() const {
                        return refine_x_down;
                }

                int32_t getRefineYUp() const {
                        return refine_y_up;
                }

                int32_t getRefineYDown() const {
                        return refine_y_down;
                }

                int32_t getRefineZUp() const {
                        return refine_z_up;
                }

                int32_t getRefineZDown() const {
                        return refine_z_down;
                }

                int32_t* getCommXUp() const {
                        return commXUp;
                }

                int32_t* getCommXDown() const {
                        return commXDown;
                }

                int32_t* getCommYUp() const {
                        return commYUp;
                }

              int32_t* getCommYDown() const {
                        return commYDown;
                }

                int32_t* getCommZUp() const {
                        return commZUp;
                }

                int32_t* getCommZDown() const {
                        return commZDown;
                }

                void setCommXUp(const int32_t x1, const int32_t x2, const int32_t x3, const int32_t x4) {
                        commXUp[0] = x1;
                        commXUp[1] = x2;
                        commXUp[2] = x3;
                        commXUp[3] = x4;
                }

                void setCommXDown(const int32_t x1, const int32_t x2, const int32_t x3, const int32_t x4) {
                        commXDown[0] = x1;
                        commXDown[1] = x2;
                        commXDown[2] = x3;
                        commXDown[3] = x4;
                }

                void setCommYUp(const int32_t x1, const int32_t x2, const int32_t x3, const int32_t x4) {
                        commYUp[0] = x1;
                        commYUp[1] = x2;
                        commYUp[2] = x3;
                        commYUp[3] = x4;
                }

                void setCommYDown(const int32_t x1, const int32_t x2, const int32_t x3, const int32_t x4) {
                        commYDown[0] = x1;
                        commYDown[1] = x2;
                        commYDown[2] = x3;
                        commYDown[3] = x4;
                }

                void setCommZUp(const int32_t x1, const int32_t x2, const int32_t x3, const int32_t x4) {
                        commZUp[0] = x1;
                        commZUp[1] = x2;
                        commZUp[2] = x3;
                        commZUp[3] = x4;
                }

                void setCommZDown(const int32_t x1, const int32_t x2, const int32_t x3, const int32_t x4) {
                        commZDown[0] = x1;
                        commZDown[1] = x2;
                        commZDown[2] = x3;
                        commZDown[3] = x4;
                }

        private:
                uint32_t blockID;
                uint32_t refinementLevel;

                int32_t refine_x_up;
                int32_t refine_x_down;
                int32_t refine_y_up;
                int32_t refine_y_down;
                int32_t refine_z_up;
                int32_t refine_z_down;

                int32_t* commXUp;
                int32_t* commXDown;
                int32_t* commYUp;
                int32_t* commYDown;
                int32_t* commZUp;
                int32_t* commZDown;
};

class Ember3DAMRGenerator : public EmberMessagePassingGenerator {

public:
	Ember3DAMRGenerator(SST::Component* owner, Params& params);
	~Ember3DAMRGenerator() {}
	void configure();
        bool generate( std::queue<EmberEvent*>& evQ );
	int32_t power3(const uint32_t expon);

	uint32_t power2(uint32_t exponent) const;
        void loadBlocks();

	uint32_t calcBlockID(const uint32_t posX, const uint32_t posY, const uint32_t posZ, const uint32_t level);
        void calcBlockLocation(const uint32_t blockID, const uint32_t blockLevel, uint32_t* posX, uint32_t* posY, uint32_t* posZ);
        bool isBlockLocal(const uint32_t bID) const;

private:
	void printBlockMap();

        std::vector<Ember3DAMRBlock*> localBlocks;
        std::map<uint32_t, uint32_t>  blockToNodeMap;
        char* blockFilePath;

        uint32_t blockCount;
        uint32_t maxLevel;
        uint32_t blocksX;
        uint32_t blocksY;
        uint32_t blocksZ;

        uint32_t rank;
        uint32_t worldSize;

        // Share these over all instances of the motif
        uint32_t peX;
        uint32_t peY;
        uint32_t peZ;

        uint32_t nsCompute;
        uint32_t nsCopyTime;

        uint32_t nx;
        uint32_t ny;
        uint32_t nz;
        uint32_t items_per_cell;
        uint32_t sizeof_cell;

        int32_t  x_down;
        int32_t  x_up;
        int32_t  y_down;
        int32_t  y_up;
        int32_t  z_down;
        int32_t  z_up;

};

}
}

#endif