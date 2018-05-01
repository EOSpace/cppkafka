/*
 * Copyright (c) 2017, Matias Fontanini
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above
 *   copyright notice, this list of conditions and the following disclaimer
 *   in the documentation and/or other materials provided with the
 *   distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
 
#include "utils/roundrobin_poll_adapter.h"

using std::string;
using std::chrono::milliseconds;
using std::make_move_iterator;

namespace cppkafka {

RoundRobinPollAdapter::RoundRobinPollAdapter(Consumer& consumer)
: consumer_(consumer),
  consumer_queue_(consumer.get_consumer_queue()) {
    // get all currently active partition assignments
    TopicPartitionList assignment = consumer_.get_assignment();
    on_assignment(assignment);
    
    // take over the assignment callback
    assignment_callback_ = consumer.get_assignment_callback();
    consumer_.set_assignment_callback([this](TopicPartitionList& partitions) {
        on_assignment(partitions);
    });
    // take over the revocation callback
    revocation_callback_ = consumer.get_revocation_callback();
    consumer_.set_revocation_callback([this](const TopicPartitionList& partitions) {
        on_revocation(partitions);
    });
    // take over the rebalance error callback
    rebalance_error_callback_ = consumer.get_rebalance_error_callback();
    consumer_.set_rebalance_error_callback([this](Error error) {
        on_rebalance_error(error);
    });
}

RoundRobinPollAdapter::~RoundRobinPollAdapter() {
    restore_forwarding();
    //reset the original callbacks
    consumer_.set_assignment_callback(assignment_callback_);
    consumer_.set_revocation_callback(revocation_callback_);
    consumer_.set_rebalance_error_callback(rebalance_error_callback_);
}

void RoundRobinPollAdapter::set_timeout(milliseconds timeout) {
    consumer_.set_timeout(timeout);
}

milliseconds RoundRobinPollAdapter::get_timeout() {
    return consumer_.get_timeout();
}

Message RoundRobinPollAdapter::poll() {
    return poll(consumer_.get_timeout());
}

Message RoundRobinPollAdapter::poll(milliseconds timeout) {
    size_t num_queues = partition_queues_.get_queues().size();
    // Always give priority to group and global events
    Message message = consumer_queue_.consume(num_queues ? milliseconds(0) : timeout);
    if (!message) {
        while (num_queues--) {
            //consume the next partition
            message = partition_queues_.get_next_queue().consume();
            if (message) {
                return message;
            }
        }
    }
    // wait on the next queue
    return partition_queues_.get_next_queue().consume(timeout);
}

MessageList RoundRobinPollAdapter::poll_batch(size_t max_batch_size) {
    return poll_batch(max_batch_size, consumer_.get_timeout());
}

MessageList RoundRobinPollAdapter::poll_batch(size_t max_batch_size, milliseconds timeout) {
    size_t num_queues = partition_queues_.get_queues().size();
    ssize_t count = max_batch_size;
    // batch from the group event queue first
    MessageList messages = consumer_queue_.consume_batch(count, num_queues ? milliseconds(0) : timeout);
    count -= messages.size();
    while ((count > 0) && (num_queues--)) {
        // batch from the next partition
        consume_batch(messages, count, milliseconds(0));
    }
    if (count > 0) {
        // wait on the next queue
        consume_batch(messages, count, timeout);
    }
    return messages;
}

void RoundRobinPollAdapter::consume_batch(MessageList& messages, ssize_t& count, milliseconds timeout)
{
    MessageList partition_messages = partition_queues_.get_next_queue().consume_batch(count, timeout);
    if (partition_messages.empty()) {
        return;
    }
    // concatenate both lists
    messages.reserve(messages.size() + partition_messages.size());
    messages.insert(messages.end(),
                    make_move_iterator(partition_messages.begin()),
                    make_move_iterator(partition_messages.end()));
    // reduce total batch count
    count -= partition_messages.size();
}

void RoundRobinPollAdapter::on_assignment(TopicPartitionList& partitions) {
    // populate partition queues
    for (const auto& partition : partitions) {
        // get the queue associated with this partition
        partition_queues_.get_queues().emplace(partition, consumer_.get_partition_queue(partition));
    }
    // reset the queue iterator
    partition_queues_.rewind();
    // call original consumer callback if any
    if (assignment_callback_) {
        assignment_callback_(partitions);
    }
}

void RoundRobinPollAdapter::on_revocation(const TopicPartitionList& partitions) {
    for (const auto& partition : partitions) {
        // get the queue associated with this partition
        auto qit = partition_queues_.get_queues().find(partition);
        if (qit != partition_queues_.get_queues().end()) {
            // restore forwarding on this queue
            qit->second.forward_to_queue(consumer_queue_);
            // remove this queue from the list
            partition_queues_.get_queues().erase(qit);
        }
    }
    // reset the queue iterator
    partition_queues_.rewind();
    // call original consumer callback if any
    if (revocation_callback_) {
        revocation_callback_(partitions);
    }
}

void RoundRobinPollAdapter::on_rebalance_error(Error error) {
    // Todo : clear partition queues ?
    // call original consumer callback if any
    if (rebalance_error_callback_) {
        rebalance_error_callback_(error);
    }
}

void RoundRobinPollAdapter::restore_forwarding() {
    // forward all partition queues
    for (const auto& toppar_queue : partition_queues_.get_queues()) {
        toppar_queue.second.forward_to_queue(consumer_queue_);
    }
}

} //cppkafka
