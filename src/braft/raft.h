// Copyright (c) 2015 Baidu.com, Inc. All Rights Reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authros: Zhangyi Chen(chenzhangyi01@baidu.com)
//          Wang,Yao(wangyao02@baidu.com)
//          Xiong,Kai(xiongkai@baidu.com)
//          Ge,Jun(gejun@baidu.com)

#ifndef BRAFT_RAFT_H
#define BRAFT_RAFT_H

#include <string>

#include <butil/logging.h>
#include <butil/iobuf.h>
#include <butil/status.h>
#include <brpc/callback.h>
#include "braft/configuration.h"
#include "braft/enum.pb.h"
#include "braft/errno.pb.h"

template <typename T> class scoped_refptr;

namespace brpc {
class Server;
}  // namespace brpc

namespace braft {

class SnapshotWriter;
class SnapshotReader;
class SnapshotHook;
class LeaderChangeContext;
class FileSystemAdaptor;
class SnapshotThrottle;
class LogStorage;

const PeerId ANY_PEER(butil::EndPoint(butil::IP_ANY, 0), 0);

// Raft-specific closure which encloses a butil::Status to report if the
// operation was successful.
// Closure 就是一个简单的 callback 接口， 提供的大部分方法都是异步的回调模式，结果通过此接口通知：
class Closure : public google::protobuf::Closure {
public:
    butil::Status& status() { return _st; }
    const butil::Status& status() const { return _st; }
    
private:
    butil::Status _st;
};

// Describe a specific error
class Error {
public:
    Error() : _type(ERROR_TYPE_NONE) {}
    Error(const Error& e) : _type(e._type), _st(e._st) {}
    ErrorType type() const { return _type; }
    const butil::Status& status() const { return _st; }
    butil::Status& status() { return _st; }
    void set_type(ErrorType type) { _type = type; }

    Error& operator=(const Error& rhs) {
        _type = rhs._type;
        _st = rhs._st;
        return *this;
    }
private:
    // Intentionally copyable
    ErrorType _type;
    butil::Status _st;
};

inline const char* errortype2str(ErrorType t) {
    switch (t) {
    case ERROR_TYPE_NONE:
        return "None";
    case ERROR_TYPE_LOG:
        return "LogError";
    case ERROR_TYPE_STABLE:
        return "StableError";
    case ERROR_TYPE_SNAPSHOT:
        return "SnapshotError";
    case ERROR_TYPE_STATE_MACHINE:
        return "StateMachineError";
    }
    return "Unknown";
}

inline std::ostream& operator<<(std::ostream& os, const Error& e) {
    os << "{type=" << errortype2str(e.type()) 
       << ", error_code=" << e.status().error_code()
       << ", error_text=`" << e.status().error_cstr()
       << "'}";
    return os;
}

/*
    Task 是用户使用 raft 最核心的类之一，用于向一个 raft 复制分组提交一个任务，这个任务提交到 leader，并复制到其他 follower 节点， Task 包括：
        butil::IOBuf* data 任务的数据，用户应当将要复制的业务数据通过一定序列化方式序列化成一个 ByteBuffer，放到 task 里。

        long expected_term = -1 任务提交时预期的 leader term，如果不提供(也就是默认值 -1 )，在任务应用到状态机之前不会检查 leader 是否发生了变更，
                            如果提供了（从状态机回调中获取，参见下文），那么在将任务应用到状态机之前，会检查 term 是否匹配，如果不匹配将拒绝该任务。

        Closure* done 任务的回调，在任务完成的时候通知此对象，无论成功还是失败。这个 closure 将在 StateMachine#onApply(iterator) 方法应用到状态机的时候，
                            可以拿到并调用，一般用于客户端应答的返回。
*/
// Basic message structure of libraft
struct Task {
    Task() : data(NULL), done(NULL), expected_term(-1) {}

    // The data applied to StateMachine
    butil::IOBuf* data;

    // Continuation when the data is applied to StateMachine or error occurs.
    Closure* done;

    // Reject this task if expected_term doesn't match the current term of
    // this Node if the value is not -1
    // Default: -1
    int64_t expected_term;
};

class IteratorImpl;

// Iterator over a batch of committed tasks
//
// Example:
// void YouStateMachine::on_apply(braft::Iterator& iter) {
//     for (; iter.valid(); iter.next()) {
//         brpc::ClosureGuard done_guard(iter.done());
//         process(iter.data());
//     }
// }
class Iterator {
    DISALLOW_COPY_AND_ASSIGN(Iterator);
public:
    // Move to the next task.
    void next();

    // Return a unique and monotonically increasing identifier of the current 
    // task:
    //  - Uniqueness guarantees that committed tasks in different peers with 
    //    the same index are always the same and kept unchanged.
    //  - Monotonicity guarantees that for any index pair i, j (i < j), task 
    //    at index |i| must be applied before task at index |j| in all the 
    //    peers from the group.
    int64_t index() const;

    // Returns the term of the leader which to task was applied to.
    int64_t term() const;

    // Return the data whose content is the same as what was passed to
    // Node::apply in the leader node.
    const butil::IOBuf& data() const;

    // If done() is non-NULL, you must call done()->Run() after applying this
    // task no matter this operation succeeds or fails, otherwise the
    // corresponding resources would leak.
    //
    // If this task is proposed by this Node when it was the leader of this 
    // group and the leadership has not changed before this point, done() is 
    // exactly what was passed to Node::apply which may stand for some 
    // continuation (such as respond to the client) after updating the 
    // StateMachine with the given task. Otherweise done() must be NULL.
    Closure* done() const;

    // Return true this iterator is currently references to a valid task, false
    // otherwise, indicating that the iterator has reached the end of this
    // batch of tasks or some error has occurred
    bool valid() const;

    // Invoked when some critical error occurred. And we will consider the last 
    // |ntail| tasks (starting from the last iterated one) as not applied. After
    // this point, no further changes on the StateMachine as well as the Node 
    // would be allowed and you should try to repair this replica or just drop 
    // it.
    //
    // If |st| is not NULL, it should describe the detail of the error.
    void set_error_and_rollback(size_t ntail = 1, const butil::Status* st = NULL);

private:
friend class FSMCaller;
    Iterator(IteratorImpl* impl) : _impl(impl) {}
    ~Iterator() {};

    // The ownership of _impl belongs to FSMCaller;
    IteratorImpl* _impl;
};

// |StateMachine| is the sink of all the events of a very raft node.
// Implement a specific StateMachine for your own business logic.
//
// NOTE: All the interfaces are not guaranteed to be thread safe and they are 
// called sequentially, saying that every single operation will block all the 
// following ones.
//注意：所有的接口都不能保证是线程安全的，它们是按顺序调用的，每一个操作都会阻塞下面的所有操作。
class StateMachine {
public:
    virtual ~StateMachine();

    // Update the StateMachine with a batch a tasks that can be accessed
    // through |iterator|.
    //
    // Invoked when one or more tasks that were passed to Node::apply have been
    // committed to the raft group (quorum of the group peers have received 
    // those tasks and stored them on the backing storage).
    //
    // Once this function returns to the caller, we will regard all the iterated
    // tasks through |iter| have been successfully applied. And if you didn't
    // apply all the the given tasks, we would regard this as a critical error
    // and report a error whose type is ERROR_TYPE_STATE_MACHINE.
    /*最核心的方法，应用任务列表到状态机，任务将按照提交顺序应用。
        请注意，当这个方法返回的时候，我们就认为这一批任务都已经成功应用到状态机上，如果你没有完全应用（比如错误、异常），
        将会被当做一个 critical 级别的错误，报告给状态机的 onError 方法，错误类型为 ERROR_TYPE_STATE_MACHINE
    */
    virtual void on_apply(::braft::Iterator& iter) = 0;

    // Invoked once when the raft node was shut down.
    // Default do nothing
    virtual void on_shutdown();

    // user defined snapshot generate function, this method will block on_apply.
    // user can make snapshot async when fsm can be cow(copy-on-write).
    // call done->Run() when snapshot finished.
    // success return 0, fail return errno
    // Default: Save nothing and returns error.
    virtual void on_snapshot_save(::braft::SnapshotWriter* writer,
                                  ::braft::Closure* done);

    // user defined snapshot load function
    // get and load snapshot
    // success return 0, fail return errno
    // Default: Load nothing and returns error.
    virtual int on_snapshot_load(::braft::SnapshotReader* reader);

    // Invoked when the belonging node becomes the leader of the group at |term|
    // Default: Do nothing
    
    virtual void on_leader_start(int64_t term);

    // Invoked when this node steps down from the leader of the replication
    // group and |status| describes detailed information
    // 当前状态机所属的 raft 节点失去 leader 资格时调用，
    // status 字段描述了详细的原因，比如主动转移 leadership、重新发生选举等
    virtual void on_leader_stop(const butil::Status& status);

    // on_error is called when a critical error was encountered, after this
    // point, no any further modification is allowed to applied to this node
    // until the error is fixed and this node restarts.
    /* 当 critical 错误发生的时候，会调用此方法，braft::Error 包含了 status 等详细的错误信息；
        当这个方法被调用后，将不允许新的任务应用到状态机，直到错误被修复并且节点被重启。因此对于任何在开发阶段发现的错误，
        都应当及时做修正，如果是 raft 的问题，请及时报告。
    */
    virtual void on_error(const ::braft::Error& e);

    // Invoked when a configuration has been committed to the group
    virtual void on_configuration_committed(const ::braft::Configuration& conf);
    virtual void on_configuration_committed(const ::braft::Configuration& conf, int64_t index);

    // this method is called when a follower stops following a leader and its leader_id becomes NULL,
    // situations including: 
    // 1. handle election_timeout and start pre_vote 
    // 2. receive requests with higher term such as vote_request from a candidate
    // or append_entries_request from a new leader
    // 3. receive timeout_now_request from current leader and start request_vote
    // the parameter stop_following_context gives the information(leader_id, term and status) about the
    // very leader whom the follower followed before.
    // User can reset the node's information as it stops following some leader.
    virtual void on_stop_following(const ::braft::LeaderChangeContext& ctx);

    // this method is called when a follower or candidate starts following a leader and its leader_id
    // (should be NULL before the method is called) is set to the leader's id,
    // situations including:
    // 1. a candidate receives append_entries from a leader
    // 2. a follower(without leader) receives append_entries from a leader
    // the parameter start_following_context gives the information(leader_id, term and status) about 
    // the very leader whom the follower starts to follow.
    // User can reset the node's information as it starts to follow some leader.
    virtual void on_start_following(const ::braft::LeaderChangeContext& ctx);
};

enum State {
    // Don't change the order if you are not sure about the usage.
    STATE_LEADER = 1,
    STATE_TRANSFERRING = 2,
    STATE_CANDIDATE = 3,
    STATE_FOLLOWER = 4,
    STATE_ERROR = 5,
    STATE_UNINITIALIZED = 6,
    STATE_SHUTTING = 7,
    STATE_SHUTDOWN = 8,
    STATE_END,
};

inline const char* state2str(State state) {
    const char* str[] = {"LEADER", "TRANSFERRING", "CANDIDATE", "FOLLOWER", 
                         "ERROR", "UNINITIALIZED", "SHUTTING", "SHUTDOWN", };
    if (state < STATE_END) {
        return str[(int)state - 1];
    } else {
        return "UNKNOWN";
    }
}

// Return true if |s| indicates the node is active
inline bool is_active_state(State s) {
    // This should be as fast as possible
    return s < STATE_ERROR;
}

// This class encapsulates the parameter of on_start_following and on_stop_following interfaces.
class LeaderChangeContext {
    //DISALLOW_COPY_AND_ASSIGN主要是把该类的拷贝构造函数和赋值构造函数改为 delete 的, 这里也是private的
    DISALLOW_COPY_AND_ASSIGN(LeaderChangeContext);
public:
    LeaderChangeContext(const PeerId& leader_id, int64_t term, const butil::Status& status)
        : _leader_id(leader_id)
        , _term(term) 
        , _st(status)
    {};
    // for on_start_following, the leader_id and term are of the new leader;
    // for on_stop_following, the leader_id and term are of the old leader.
    const PeerId& leader_id() const { return _leader_id; }
    int64_t term() const { return _term; }
    // return the information about why on_start_following or on_stop_following is called.
    const butil::Status& status() const { return _st; }
        
private:
    PeerId _leader_id;
    int64_t _term;
    butil::Status _st;
};

inline std::ostream& operator<<(std::ostream& os, const LeaderChangeContext& ctx) {
    os << "{ leader_id=" << ctx.leader_id()
       << ", term=" << ctx.term()
       << ", status=" << ctx.status()
       << "}";
    return os;
}

class UserLog {
    DISALLOW_COPY_AND_ASSIGN(UserLog);
public:
    UserLog() {};
    UserLog(int64_t log_index, const butil::IOBuf& log_data)
        : _index(log_index)
        , _data(log_data)
    {};
    int64_t log_index() const { return _index; }
    const butil::IOBuf& log_data() const { return _data; }
    void set_log_index(const int64_t log_index) { _index = log_index; }
    void set_log_data(const butil::IOBuf& log_data) { _data = log_data; }
    void reset() {
        _index = 0;
        _data.clear();
    }

private:
    int64_t _index;
    butil::IOBuf _data;
};

inline std::ostream& operator<<(std::ostream& os, const UserLog& user_log) {
    os << "{user_log: index=" << user_log.log_index()
       << ", data size=" << user_log.log_data().size()
       << "}";
    return os;
}

// Status of a peer
struct PeerStatus {
    PeerStatus()
        : valid(false), installing_snapshot(false), next_index(0)
        , last_rpc_send_timestamp(0), flying_append_entries_size(0)
        , readonly_index(0), consecutive_error_times(0)
    {}

    bool    valid;
    bool    installing_snapshot;
    int64_t next_index;
    int64_t last_rpc_send_timestamp;
    int64_t flying_append_entries_size;
    int64_t readonly_index;
    int     consecutive_error_times;
};

// Status of Node
struct NodeStatus {
    typedef std::map<PeerId, PeerStatus> PeerStatusMap;

    NodeStatus()
        : state(STATE_END), readonly(false), term(0), committed_index(0), known_applied_index(0)
        , pending_index(0), pending_queue_size(0), applying_index(0), first_index(0)
        , last_index(-1), disk_index(0)
    {}

    State state;
    PeerId peer_id;
    PeerId leader_id;
    bool readonly;
    int64_t term;
    int64_t committed_index;
    int64_t known_applied_index;

    // The start index of the logs waiting to be committed.
    // If the value is 0, means no pending logs.
    // 
    // WARNING: if this value is not 0, and keep the same in a long time,
    // means something happend to prevent the node to commit logs in a
    // large probability, and users should check carefully to find out
    // the reasons.
    int64_t pending_index;

    // How many pending logs waiting to be committed.
    // 
    // WARNING: too many pending logs, means the processing rate can't catup with
    // the writing rate. Users can consider to slow down the writing rate to avoid
    // exhaustion of resources.
    int64_t pending_queue_size;

    // The current applying index. If the value is 0, means no applying log.
    //
    // WARNING: if this value is not 0, and keep the same in a long time, means
    // the apply thread hung, users should check if a deadlock happend, or some
    // time-consuming operations is handling in place.
    int64_t applying_index;

    // The first log of the node, including the logs in memory and disk.
    int64_t first_index;

    // The last log of the node, including the logs in memory and disk.
    int64_t last_index;

    // The max log in disk.
    int64_t disk_index;

    // Stable followers are peers in current configuration.
    // If the node is not leader, this map is empty.
    PeerStatusMap stable_followers;

    // Unstable followers are peers not in current configurations. For example,
    // if a new peer is added and not catchup now, it's in this map.
    PeerStatusMap unstable_followers;
};

// State of a lease. Following is a typical lease state change diagram:
// 
// event:                 become leader                 become follower
//                        ^           on leader start   ^   on leader stop
//                        |           ^                 |   ^
// time:        ----------|-----------|-----------------|---|-------
// lease state:   EXPIRED | NOT_READY |      VALID      |  EXPIRED  
// 
enum LeaseState {
    // Lease is disabled, this state will only be returned when
    // |raft_enable_leader_lease == false|.
    LEASE_DISABLED = 1,

    // Lease is expired, this node is not leader any more.
    LEASE_EXPIRED = 2,

    // This node is leader, but we are not sure the data is up to date. This state
    // continue until |on_leader_start| or the leader step down.
    LEASE_NOT_READY = 3,

    // Lease is valid.
    LEASE_VALID = 4,
};

// Status of a leader lease.
struct LeaderLeaseStatus {
    LeaderLeaseStatus()
        : state(LEASE_DISABLED), term(0), lease_epoch(0)
    {}

    LeaseState state;

    // These followering fields are only meaningful when |state == LEASE_VALID|.
    
    // The term of this lease
    int64_t term;

    // A specific term may have more than one lease, when transfer leader timeout
    // happen. Lease epoch will be guranteed to be monotinically increase, in the
    // life cycle of a node.
    int64_t lease_epoch;
};

struct NodeOptions {
    // A follower would become a candidate if it doesn't receive any message 
    // from the leader in |election_timeout_ms| milliseconds
    // Default: 1000 (1s)
    // 一个 follower 当超过这个设定时间没有收到 leader 的消息后，变成 candidate 节点的时间。
    // leader 会在 electionTimeoutMs 时间内向 follower 发消息（心跳或者复制日志），如果没有收到，
    // follower 就需要进入 candidate状态，发起选举或者等待新的 leader 出现，默认1秒。
    int election_timeout_ms; //follower to candidate timeout

    // Max clock drift time. It will be used to keep the safety of leader lease.
    // Default: 1000 (1s)
    int max_clock_drift_ms;

    // A snapshot saving would be triggered every |snapshot_interval_s| seconds
    // if this was reset as a positive number
    // If |snapshot_interval_s| <= 0, the time based snapshot would be disabled.
    //
    // Default: 3600 (1 hour)
    // 自动 Snapshot 间隔时间，默认一个小时
    int snapshot_interval_s;

    // We will regard a adding peer as caught up(赶上) if the margin between the
    // last_log_index of this peer and the last_log_index of leader is less than
    // |catchup_margin|
    //
    // Default: 1000
    int catchup_margin;

    // If node is starting from an empty environment (both LogStorage and
    // SnapshotStorage are empty), it would use |initial_conf| as the
    // configuration of the group, otherwise it would load configuration from
    // the existing environment.
    //
    // Default: A empty group
    // 当节点是从一个空白状态启动（snapshot和log存储都为空），那么他会使用这个初始配置作为 raft group
    // 的配置启动，否则会从存储中加载已有配置。
    Configuration initial_conf;

    // Run the user callbacks and user closures in pthread rather than bthread
    // 
    // Default: false
    bool usercode_in_pthread;

    // The specific StateMachine implemented your business logic, which must be
    // a valid instance.
    // 最核心的，属于本 raft 节点的应用状态机实例。
    StateMachine* fsm;

    // If |node_owns_fsm| is true. |fms| would be destroyed when the backing
    // Node is no longer referenced.
    //
    // Default: false
    bool node_owns_fsm;

    // The specific LogStorage implemented at the bussiness layer, which should be a valid
    // instance, otherwise use SegmentLogStorage by default.
    //
    // Default: null
    LogStorage* log_storage;

    // If |node_owns_log_storage| is true. |log_storage| would be destroyed when
    // the backing Node is no longer referenced.
    //
    // Default: true
    bool node_owns_log_storage;

    // Describe a specific LogStorage in format ${type}://${parameters}
    // It's valid iff |log_storage| is null
    // Raft 节点的日志存储路径，必须有
    std::string log_uri;

    // Describe a specific RaftMetaStorage in format ${type}://${parameters}
    // Three types are provided up till now:
    // 1. type=local
    //     FileBasedSingleMetaStorage(old name is LocalRaftMetaStorage) will be
    //     used, which is based on protobuf file and manages stable meta of
    //     only one Node
    //     typical format: local://${node_path}
    // 2. type=local-merged
    //     KVBasedMergedMetaStorage will be used, whose under layer is based
    //     on KV storage and manages a batch of Nodes one the same disk. It's 
    //     designed to solve performance problems caused by lots of small
    //     synchronous IO during leader electing, when there are huge number of
    //     Nodes in Multi-raft situation.
    //     typical format: local-merged://${disk_path}
    // 3. type=local-mixed
    //     MixedMetaStorage will be used, which will double write the above
    //     two types of meta storages when upgrade an downgrade.
    //     typical format:
    //     local-mixed://merged_path=${disk_path}&&single_path=${node_path}
    // 
    // Upgrade and Downgrade steps:
    //     upgrade from Single to Merged: local -> mixed -> merged
    //     downgrade from Merged to Single: merged -> mixed -> local
    // Raft 节点的元信息存储路径，必须有
    std::string raft_meta_uri;

    // Describe a specific SnapshotStorage in format ${type}://${parameters}
    // Raft 节点的 snapshot 存储路径，可选，不提供就关闭了 snapshot 功能。
    std::string snapshot_uri;

    // If enable, we will filter duplicate files before copy remote snapshot,
    // to avoid useless transmission. Two files in local and remote are duplicate,
    // only if they has the same filename and the same checksum (stored in file meta).
    // Default: false
    bool filter_before_copy_remote;

    // If non-null, we will pass this snapshot_file_system_adaptor to SnapshotStorage
    // Default: NULL
    scoped_refptr<FileSystemAdaptor>* snapshot_file_system_adaptor;    
    
    // If non-null, we will pass this snapshot_throttle to SnapshotExecutor
    // Default: NULL
    scoped_refptr<SnapshotThrottle>* snapshot_throttle;

    // If true, RPCs through raft_cli will be denied.
    // Default: false
    // 是否关闭 Cli 服务，默认不关闭
    bool disable_cli;

    // Construct a default instance
    NodeOptions();
};

inline NodeOptions::NodeOptions() 
    : election_timeout_ms(1000)
    , max_clock_drift_ms(1000)
    , snapshot_interval_s(3600)
    , catchup_margin(1000)
    , usercode_in_pthread(false)
    , fsm(NULL)
    , node_owns_fsm(false)
    , log_storage(NULL)
    , node_owns_log_storage(true)
    , filter_before_copy_remote(false)
    , snapshot_file_system_adaptor(NULL)
    , snapshot_throttle(NULL)
    , disable_cli(false)
{}

class NodeImpl;
class Node {
public:
    /*  一个Node代表了一个RAFT实例， Node的ID由两个部分组成:
        GroupId: 为一个string, 表示这个复制组的ID.
        PeerId, 结构是一个EndPoint表示对外服务的端口, 外加一个index(默认为0). 其中index的作用是让不同的副本能运行在同一个进程内, 在下面几个场景中，这个值不能忽略:
    */
    Node(const GroupId& group_id, const PeerId& peer_id);
    virtual ~Node();

    // get node id
    NodeId node_id();

    // get leader PeerId, for redirect
    //获取当前 raft group 的 leader peerId，如果未知，返回 null
    PeerId leader_id();

    // Return true if this is the leader of the belonging group
    bool is_leader();

    // Return true if this is the leader, and leader lease is valid. It's always
    // false when |raft_enable_leader_lease == false|.
    // In the follwing situations, the returned true is unbeleivable:
    //    -  Not all nodes in the raft group set |raft_enable_leader_lease| to true,
    //       and tranfer leader/vote interfaces are used;
    //    -  In the raft group, the value of |election_timeout_ms| in one node is larger
    //       than |election_timeout_ms + max_clock_drift_ms| in another peer.
    bool is_leader_lease_valid();

    // Get leader lease status for more complex checking
    void get_leader_lease_status(LeaderLeaseStatus* status);

    // init node
    int init(const NodeOptions& options);

    // shutdown local replica.
    // done is user defined function, maybe response to client or clean some resource
    // [NOTE] code after apply can't access resource in done
    //前者用于停止一个 raft 节点，后者可以在 shutdown 调用后等待停止过程结束。
    void shutdown(Closure* done);

    // Block the thread until the node is successfully stopped.
    void join();

    // [Thread-safe and wait-free]
    // apply task to the replicated-state-machine
    //
    // About the ownership:
    // |task.data|: for the performance consideration, we will take away the 
    //              content. If you want keep the content, copy it before call
    //              this function
    // |task.done|: If the data is successfully committed to the raft group. We
    //              will pass the ownership to StateMachine::on_apply.
    //              Otherwise we will specify the error and call it.
    //
    //提交一个新任务到 raft group，此方法是线程安全并且非阻塞，无论任务是否成功提交到 raft group，
    //都会通过 task 关联的 closure done 通知到。如果当前节点不是 leader，会直接失败通知 done closure。
    void apply(const Task& task);

    // list peers of this raft group, only leader retruns ok
    // [NOTE] when list_peers concurrency with add_peer/remove_peer, maybe return peers is staled.
    // because add_peer/remove_peer immediately modify configuration in memory
    butil::Status list_peers(std::vector<PeerId>* peers);

    // Add a new peer to the raft group. done->Run() would be invoked after this
    // operation finishes, describing the detailed result.
    void add_peer(const PeerId& peer, Closure* done);

    // Remove the peer from the raft group. done->Run() would be invoked after
    // this operation finishes, describing the detailed result.
    void remove_peer(const PeerId& peer, Closure* done);

    // Change the configuration of the raft group to |new_peers| , done->Run()
    // would be invoked after this operation finishes, describing the detailed
    // result.
    void change_peers(const Configuration& new_peers, Closure* done);

    // Reset the configuration of this node individually, without any repliation
    // to other peers before this node beomes the leader. This function is
    // supposed to be inovoked when the majority of the replication group are
    // dead and you'd like to revive the service in the consideration of
    // availability.
    // Notice that neither consistency nor consensus are guaranteed in this
    // case, BE CAREFULE when dealing with this method.
    butil::Status reset_peers(const Configuration& new_peers);

    // Start a snapshot immediately if possible. done->Run() would be invoked
    // when the snapshot finishes, describing the detailed result.
    //触发当前节点执行一次 snapshot 保存操作，结果通过 done 通知
    void snapshot(Closure* done);

    // user trigger vote
    // reset election_timeout, suggest some peer to become the leader in a
    // higher probability
    butil::Status vote(int election_timeout);

    // Reset the |election_timeout_ms| for the very node, the |max_clock_drift_ms|
    // is also adjusted to keep the sum of |election_timeout_ms| and |the max_clock_drift_ms|
    // unchanged.
    butil::Status reset_election_timeout_ms(int election_timeout_ms);

    // Forcely reset |election_timeout_ms| and |max_clock_drift_ms|. It may break
    // leader lease safety, should be careful.
    // Following are suggestions for you to change |election_timeout_ms| safely.
    // 1. Three steps to safely upgrade |election_timeout_ms| to a larger one:
    //     - Enlarge |max_clock_drift_ms| in all peers to make sure
    //       |old election_timeout_ms + new max_clock_drift_ms| larger than
    //       |new election_timeout_ms + old max_clock_drift_ms|.
    //     - Wait at least |old election_timeout_ms + new max_clock_drift_ms| times to make
    //       sure all previous elections complete.
    //     - Upgrade |election_timeout_ms| to new one, meanwhiles |max_clock_drift_ms|
    //       can set back to the old value.
    // 2. Three steps to safely upgrade |election_timeout_ms| to a smaller one:
    //     - Adjust |election_timeout_ms| and |max_clock_drift_ms| at the same time,
    //       to make the sum of |election_timeout_ms + max_clock_drift_ms| unchanged.
    //     - Wait at least |election_timeout_ms + max_clock_drift_ms| times to make
    //       sure all previous elections complete.
    //     - Upgrade |max_clock_drift_ms| back to the old value.
    void reset_election_timeout_ms(int election_timeout_ms, int max_clock_drift_ms);

    // Try transferring leadership to |peer|.
    // If peer is ANY_PEER, a proper follower will be chosen as the leader for
    // the next term.
    // Returns 0 on success, -1 otherwise.
    int transfer_leadership_to(const PeerId& peer);

    // Read the first committed user log from the given index.
    // Return OK on success and user_log is assigned with the very data. Be awared
    // that the user_log may be not the exact log at the given index, but the
    // first available user log from the given index to last_committed_index.
    // Otherwise, appropriate errors are returned:
    //     - return ELOGDELETED when the log has been deleted;
    //     - return ENOMOREUSERLOG when we can't get a user log even reaching last_committed_index.
    // [NOTE] in consideration of safety, we use last_applied_index instead of last_committed_index 
    // in code implementation.
    butil::Status read_committed_user_log(const int64_t index, UserLog* user_log);

    // Get the internal status of this node, the information is mostly the same as we
    // see from the website.
    void get_status(NodeStatus* status);

    // Make this node enter readonly mode.
    // Readonly mode should only be used to protect the system in some extreme cases.
    // For example, in a storage system, too many write requests flood into the system
    // unexpectly, and the system is in the danger of exhaust capacity. There's not enough
    // time to add new machines, and wait for capacity balance. Once many disks become
    // full, quorum dead happen to raft groups. One choice in this example is readonly
    // mode, to let leader reject new write requests, but still handle reads request,
    // and configuration changes.
    // If a follower become readonly, the leader stop replicate new logs to it. This
    // may cause the data far behind the leader, in the case that the leader is still
    // writable. After the follower exit readonly mode, the leader will resume to
    // replicate missing logs.
    // A leader is readonly, if the node itself is readonly, or writable nodes (nodes that
    // are not marked as readonly) in the group is less than majority. Once a leader become
    // readonly, no new users logs will be acceptted.
    void enter_readonly_mode();

    // Node leave readonly node.
    void leave_readonly_mode();

    // Check if this node is readonly.
    // There are two situations that if a node is readonly:
    //      - This node is marked as readonly, by calling enter_readonly_mode();
    //      - This node is a leader, and the count of writable nodes in the group
    //        is less than the majority.
    bool readonly();

private:
    NodeImpl* _impl;
};

struct BootstrapOptions {

    // Containing the initial member of this raft group
    // Default: empty conf
    Configuration group_conf;

    // The index of the last index which the dumping snapshot contains
    // Default: 0
    int64_t last_log_index;

    // The specific StateMachine which is going to dump the first snapshot 
    // If last_log_index isn't 0, fsm must be a valid instance.
    // Default: NULL
    StateMachine* fsm;

    // If |node_owns_fsm| is true. |fsm| would be destroyed when the backing
    // Node is no longer referenced.
    //
    // Default: false
    bool node_owns_fsm;

    // Run the user callbacks and user closures in pthread rather than bthread
    // 
    // Default: false
    bool usercode_in_pthread;

    // Describe a specific LogStorage in format ${type}://${parameters}
    std::string log_uri;

    // Describe a specific RaftMetaStorage in format ${type}://${parameters}
    std::string raft_meta_uri;

    // Describe a specific SnapshotStorage in format ${type}://${parameters}
    std::string snapshot_uri;

    // Construct default options
    BootstrapOptions();

};

// Bootstrap a non-empty raft node, 
int bootstrap(const BootstrapOptions& options);

// Attach raft services to |server|, this makes the raft services share the same
// listening address with the user services.
//
// NOTE: Now we only allow the backing Server to be started with a specific
// listen address, if the Server is going to be started from a range of ports, 
// the behavior is undefined.
// Returns 0 on success, -1 otherwise.
int add_service(brpc::Server* server, const butil::EndPoint& listen_addr);
int add_service(brpc::Server* server, int port);
int add_service(brpc::Server* server, const char* listen_ip_and_port);

// GC
struct GCOptions {
    // Versioned-groupid of this raft instance. 
    // Version is necessary because instance with the same groupid may be created 
    // again very soon after destroyed.
    VersionedGroupId vgid;
    std::string log_uri;
    std::string raft_meta_uri;
    std::string snapshot_uri;
};

// TODO What if a disk is dropped and added again without released from 
// global_mss_manager? It seems ok because all the instance on that disk would
// be destroyed before dropping the disk itself, so there would be no garbage. 
// 
// GC the data of a raft instance when destroying the instance by some reason.
//
// Returns 0 on success, -1 otherwise.
int gc_raft_data(const GCOptions& gc_options);

}  //  namespace braft

#endif //BRAFT_RAFT_H
