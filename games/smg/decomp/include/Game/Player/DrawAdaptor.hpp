#pragma once

#include "Game/NameObj/NameObj.hpp"

class DrawAdaptor : public NameObj {
public:
    DrawAdaptor(const MR::FunctorBase& rFunc, int drawType, const char* pName);

    virtual void draw() const;

private:
    /* 0x0C */ const MR::FunctorBase* mFunc;
};
