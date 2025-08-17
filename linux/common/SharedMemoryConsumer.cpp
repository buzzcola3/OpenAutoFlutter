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


#include "SharedMemoryConsumer.hpp"
#include <iostream>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <thread>
#include <chrono>

// Initialize static member
volatile std::sig_atomic_t SharedMemoryConsumer::running_ = 1;

void SharedMemoryConsumer::signalHandler(int signal) {
    running_ = 0;
}

SharedMemoryConsumer::SharedMemoryConsumer(const std::string& shmName, const std::string& semName, size_t shmSize, BufferCallback callback, unsigned int polling_ms)
        : shmName_(shmName),
            semName_(semName),
            shmSize_(shmSize),
            polling_ms_(polling_ms),
            currentState_(State::CONNECTING),
            semaphore_(SEM_FAILED),
            shm_fd_(-1),
            ptr_(MAP_FAILED),
            buffer_(nullptr),
            producerAliveCheck_(0),
            onNewBuffer_(callback)
{
    // Setup signal handling
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SharedMemoryConsumer::signalHandler;
    sa.sa_flags &= ~SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

SharedMemoryConsumer::~SharedMemoryConsumer() {
    // Final cleanup
    if (ptr_ != MAP_FAILED) munmap(ptr_, shmSize_);
    if (shm_fd_ != -1) close(shm_fd_);
    if (semaphore_ != SEM_FAILED) sem_close(semaphore_);
    std::cout << "Consumer has exited." << std::endl;
}

void SharedMemoryConsumer::run() {
    while (running_) {
        switch (currentState_) {
            case State::CONNECTING:
                handleConnecting();
                break;
            case State::POLLING:
                handlePolling();
                break;
            case State::SHUTDOWN:
                handleShutdown();
                break;
        }
    }
}

void SharedMemoryConsumer::handleConnecting() {
    std::cout << "[State: CONNECTING] Waiting for shared resources..." << std::endl;
    shm_fd_ = shm_open(shmName_.c_str(), O_RDONLY, 0666);
    if (shm_fd_ != -1) {
        semaphore_ = sem_open(semName_.c_str(), 0);
        if (semaphore_ != SEM_FAILED) {
            ptr_ = mmap(nullptr, shmSize_, PROT_READ, MAP_SHARED, shm_fd_, 0);
            if (ptr_ != MAP_FAILED) {
                buffer_ = static_cast<unsigned char*>(ptr_);
                std::cout << "[State: CONNECTING] Successfully connected." << std::endl;
                producerAliveCheck_ = 0; // Reset counter
                currentState_ = State::POLLING; // Transition
            } else {
                perror("mmap failed");
                currentState_ = State::SHUTDOWN;
            }
        } else {
            close(shm_fd_);
        }
    }

    if (currentState_ == State::CONNECTING) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void SharedMemoryConsumer::handlePolling() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    // Use the polling_ms_ member variable to calculate the timeout
    long ns_to_add = polling_ms_ * 1000000;
    ts.tv_nsec += ns_to_add;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    if (sem_timedwait(semaphore_, &ts) == -1) {
        if (errno == EINTR) return; // Let the main loop check `running`
        if (errno == ETIMEDOUT) {
            producerAliveCheck_++;
            if (producerAliveCheck_ > 100) {
                std::cout << "[State: POLLING] Producer not detected for 100 cycles." << std::endl;
                currentState_ = State::CONNECTING;
            }
            return;
        }
        perror("sem_timedwait failed");
        currentState_ = State::SHUTDOWN;
    } else {
        producerAliveCheck_ = 0;

        //std::cout << "first byte: " << static_cast<int>(buffer_[0]) << std::endl;

        if (onNewBuffer_) {
            onNewBuffer_(buffer_, shmSize_);
        }
    }
}

void SharedMemoryConsumer::handleShutdown() {
    std::cout << "[State: SHUTDOWN] Shutting down." << std::endl;
    running_ = 0;
}