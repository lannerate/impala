// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "util/hdfs-bulk-ops.h"

#include <vector>
#include <boost/foreach.hpp>

#include "util/hdfs-util.h"

using namespace std;
using namespace impala;
using namespace boost;

HdfsOp::HdfsOp(HdfsOpType op, const std::string& src, HdfsOperationSet* op_set)
    : op_(op), src_(src), op_set_(op_set) {
  DCHECK(op != RENAME);
  DCHECK(!src_.empty());
}

HdfsOp::HdfsOp(HdfsOpType op, const std::string& src, const std::string& dst,
    HdfsOperationSet* op_set) : op_(op), src_(src), dst_(dst), op_set_(op_set) {
  DCHECK(op == RENAME);
  DCHECK(!src_.empty());
  DCHECK(!dst_.empty());
}

// Required for ThreadPool
HdfsOp::HdfsOp() { }

void HdfsOp::Execute() const {
  if (op_set_->ShouldAbort()) return;
  int err = 0;
  hdfsFS* hdfs_connection = op_set_->hdfs_connection();
  switch (op_) {
    case DELETE:
      err = hdfsDelete(*hdfs_connection, src_.c_str(), 1);
      break;
    case CREATE_DIR:
      err = hdfsCreateDirectory(*hdfs_connection, src_.c_str());
      break;
    case RENAME:
      err = hdfsRename(*hdfs_connection, src_.c_str(), dst_.c_str());
      break;
    case DELETE_THEN_CREATE:
      err = hdfsDelete(*hdfs_connection, src_.c_str(), 1);
      if (err != -1) err = hdfsCreateDirectory(*hdfs_connection, src_.c_str());
      break;
  }

  if (err == -1) {
    stringstream ss;
    ss << "Hdfs op (";
    switch (op_) {
      case DELETE:
        ss << "DELETE " << src_;
        break;
      case CREATE_DIR:
        ss << "CREATE_DIR " << src_;
        break;
      case RENAME:
        ss << "RENAME " << src_ << " TO " << dst_;
        break;
      case DELETE_THEN_CREATE:
        ss << "DELETE_THEN_CREATE " << src_;
        break;
    }
    ss << ") failed, error was: ";

    op_set_->AddError(AppendHdfsErrorMessage(ss.str()), this);
  }

  op_set_->MarkOneOpDone();
}

// Utility method to convert from a thread-pool signature to HdfsOp::Execute
void HdfsThreadPoolHelper(int thread_id, const HdfsOp& op) {
  op.Execute();
}

// Factory method to create a new thread pool (required because only this file has access
// to the definition of HdfsThreadPoolHelper).
HdfsOpThreadPool* impala::CreateHdfsOpThreadPool(const string& name, uint32_t num_threads,
    uint32_t max_queue_length) {
  return new HdfsOpThreadPool(name, "hdfs-worker", num_threads,
      max_queue_length, &HdfsThreadPoolHelper);
}

HdfsOperationSet::HdfsOperationSet(hdfsFS* hdfs_connection)
    : num_ops_(0L), hdfs_connection_(hdfs_connection) {
}

bool HdfsOperationSet::Execute(ThreadPool<HdfsOp>* pool,
    bool abort_on_error) {
  {
    lock_guard<mutex> l(errors_lock_);
    abort_on_error_ = abort_on_error;
  }
  int64_t num_ops = ops_.size();
  if (num_ops == 0) return true;
  num_ops_ += num_ops;

  BOOST_FOREACH(const HdfsOp& op, ops_) {
    pool->Offer(op);
  }
  return promise_.Get();
}

void HdfsOperationSet::Add(HdfsOpType op, const std::string& src) {
  ops_.push_back(HdfsOp(op, src, this));
}

void HdfsOperationSet::Add(HdfsOpType op, const std::string& src,
    const std::string& dst) {
  ops_.push_back(HdfsOp(op, src, dst, this));
}

void HdfsOperationSet::AddError(const string& err, const HdfsOp* op) {
  lock_guard<mutex> l(errors_lock_);
  errors_.push_back(make_pair(op, err));
}

void HdfsOperationSet::MarkOneOpDone() {
  if (num_ops_.UpdateAndFetch(-1) == 0) {
    promise_.Set(errors().size() == 0);
  }
}

bool HdfsOperationSet::ShouldAbort() {
  lock_guard<mutex> l(errors_lock_);
  return abort_on_error_ && !errors_.empty();
}