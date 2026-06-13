
#pragma once

namespace coro {

// Cooperative yield + cancellation/timeout check. Call inside long CPU loops.
// Reposts to the event loop (giving other coroutines a turn) and throws
// CancelError / TimeoutError if the coroutine has been cancelled or the nearest
// deadline elapsed. Same contract as before, now implemented on asio::post.
void CheckPoint();

}
