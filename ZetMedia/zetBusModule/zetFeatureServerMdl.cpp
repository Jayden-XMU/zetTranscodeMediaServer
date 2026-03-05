#include <iostream>
#include "zetFeatureServerMdl.h"
using namespace std;

zetFeatureServerMdl::zetFeatureServerMdl() {
   this->init();
}

void zetFeatureServerMdl::init() {
   LOG(LOG_DEBUG, "called, and exit");
}

bool zetFeatureServerMdl::parseCmd(void* cmd) {
    LOG(LOG_DEBUG, "called, and exit...");
    return ZET_TRUE;
}

INT32 zetFeatureServerMdl::process(void* msg) {
    LOG(LOG_DEBUG, "called, and exit...");
    return ZET_OK;
}

INT32 zetFeatureServerMdl::processUpLayerCmd(const char* params, double seekTime) {
    int ret = ZET_NOK;
    LOG(LOG_DEBUG, " called, and exit!!!");
    return ret;
}

void zetFeatureServerMdl::release() {
    LOG(LOG_DEBUG, "called, and exit...");
}

zetFeatureServerMdl::~zetFeatureServerMdl() {
    this->release();
}


