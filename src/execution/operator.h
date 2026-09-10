#pragma once

#include "vector/vector.h"

namespace simple_olap {

// Pull-based, batch-at-a-time Volcano operator.
// Contract:
//   Init() exactly once before Next().
//   Next() == true  -> a non-empty active batch is available.
//   Next() == false -> EOF; caller must not read output.
//   A returned batch is valid until the next Next() call on this operator tree.
class Operator {
  public:
    virtual ~Operator() = default;
    virtual void Init() = 0;
    virtual bool Next(VectorBatch& batch) = 0;
};

} // namespace simple_olap
