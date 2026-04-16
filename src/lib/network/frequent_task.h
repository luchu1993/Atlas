#ifndef ATLAS_LIB_NETWORK_FREQUENT_TASK_H_
#define ATLAS_LIB_NETWORK_FREQUENT_TASK_H_

namespace atlas {

class FrequentTask {
 public:
  virtual ~FrequentTask() = default;
  virtual void DoTask() = 0;
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_FREQUENT_TASK_H_
