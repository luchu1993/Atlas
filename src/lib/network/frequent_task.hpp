#pragma once

namespace atlas
{

class FrequentTask
{
public:
    virtual ~FrequentTask() = default;
    virtual void do_task() = 0;
};

}  // namespace atlas
