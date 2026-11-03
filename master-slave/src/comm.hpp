#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// In-process stand-in for the MPI point-to-point calls the master-slave
// protocol used (MPI_Send / MPI_Isend / MPI_Recv / MPI_Iprobe on
// MPI_COMM_WORLD): rank 0 is the master, ranks 1..N the workers, each
// running in its own thread with its own mailbox.
//
// - send() never blocks: the payload is moved into the receiver's mailbox
//   (so it covers both MPI_Send and MPI_Isend; there is nothing to wait for).
// - recv() blocks until a message from `source` with `tag` is there.
// - try_recv_any() is MPI_Iprobe(MPI_ANY_SOURCE, tag) + MPI_Recv in one step.
// - Between one sender and one receiver, messages with the same tag arrive
//   in send order, as MPI guarantees.
//
// Messages carry serialized strings only, so no Solution (whose LocalRc
// reference count is not atomic) is ever shared between threads.
class Comm {
public:
    struct Message {
        int         source = 0;
        int         tag    = 0;
        bool        empty  = false;  // "no payload" signal (was size == -2)
        std::string payload;
    };

    explicit Comm(int world_size) : boxes_(static_cast<std::size_t>(world_size)) {}

    int size() const { return static_cast<int>(boxes_.size()); }

    void send(int source, int dest, int tag, std::string payload, bool empty = false)
    {
        Mailbox& box = boxes_[static_cast<std::size_t>(dest)];
        {
            std::lock_guard<std::mutex> lock(box.mutex);
            box.queue.push_back({source, tag, empty, std::move(payload)});
        }
        box.ready.notify_all();
    }

    Message recv(int self, int source, int tag)
    {
        Mailbox& box = boxes_[static_cast<std::size_t>(self)];
        std::unique_lock<std::mutex> lock(box.mutex);
        for (;;) {
            for (auto it = box.queue.begin(); it != box.queue.end(); ++it) {
                if (it->source == source && it->tag == tag) {
                    Message m = std::move(*it);
                    box.queue.erase(it);
                    return m;
                }
            }
            box.ready.wait(lock);
        }
    }

    std::optional<Message> try_recv_any(int self, int tag)
    {
        Mailbox& box = boxes_[static_cast<std::size_t>(self)];
        std::lock_guard<std::mutex> lock(box.mutex);
        for (auto it = box.queue.begin(); it != box.queue.end(); ++it) {
            if (it->tag == tag) {
                Message m = std::move(*it);
                box.queue.erase(it);
                return m;
            }
        }
        return std::nullopt;
    }

private:
    struct Mailbox {
        std::mutex              mutex;
        std::condition_variable ready;
        std::deque<Message>     queue;
    };
    std::vector<Mailbox> boxes_;
};
