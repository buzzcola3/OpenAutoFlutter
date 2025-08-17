// AV consumer public API
#pragma once

#ifdef __cplusplus

#include <cstdint> // uint8_t
#include <cstddef> // size_t

// A simple wrapper that starts two background consumers for video and audio
// shared memory buffers, and prints when data arrives.
class AVConsumer {
public:
	AVConsumer();
	~AVConsumer();

	// Start background threads consuming SHM buffers.
	void start();

	// Join background threads (blocks until they exit).
	void join();

	// Access last decoded YUV420P buffer (if any). Returns false if none.
	bool get_last_yuv420p(const uint8_t*& data, int& width, int& height, size_t& size_bytes) const;

	// Returns true if a new decoded frame is available since last mark_frame_consumed().
	bool is_new_frame_available() const;
	// Mark the current frame as consumed (clears the new frame flag).
	void mark_frame_consumed();

private:
	struct Impl;
	Impl* impl_;
};

extern "C" {
// Convenience C entry to run the consumer synchronously (start + join).
void av_consumer_run();
}

#endif // __cplusplus
