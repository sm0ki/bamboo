#pragma once
#include "leveldb/db.h"
#include "common.hpp"
#include "block.hpp"

class BlockStore {
    public:
        BlockStore();
        ~BlockStore();
        void init(string path);
        bool hasBlock(int blockId);
        Block getBlock(int blockId);
        void setBlock(Block& b);
    protected:
        void clearDB();
        leveldb::DB *db;
        size_t numBlocks;
        string path;
};