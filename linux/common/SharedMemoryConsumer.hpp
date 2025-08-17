/*
 *  This file is part of OpenAutoCore project.
 *  Copyright (C) 2025 buzzcola3 (Samuel Betak)
 *
 *  OpenAutoCore is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  OpenAutoCore is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with OpenAutoCore. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef SHARED_MEMORY_CONSUMER_HPP
#define SHARED_MEMORY_CONSUMER_HPP

#include <string>
#include <csignal>
#include <semaphore.h>
#include <functional>

class SharedMemoryConsumer {
public:
    // Define the callback function signature that will be triggered on a new buffer
    using BufferCallback = std::function<void(const unsigned char* buffer, size_t size)>;

    SharedMemoryConsumer(const std::string& shmName, const std::string& semName, size_t shmSize, BufferCallback callback, unsigned int polling_ms = 10);
    ~SharedMemoryConsumer();
    void run();

private:
    enum class State {
        CONNECTING,
        POLLING,
        SHUTDOWN
    };

    void handleConnecting();
    void handlePolling();
    void handleShutdown();

    static void signalHandler(int signal);

    const std::string shmName_;
    const std::string semName_;
    const size_t shmSize_;
    const unsigned int polling_ms_;

    State currentState_;
    sem_t* semaphore_;
    int shm_fd_;
    void* ptr_;
    unsigned char* buffer_;
    unsigned int producerAliveCheck_;

    // Member to hold the callback function
    BufferCallback onNewBuffer_;

    static volatile std::sig_atomic_t running_;
};

#endif // SHARED_MEMORY_CONSUMER_HPP