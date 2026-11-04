#pragma once

namespace fc_plugins::galaxy_buddies {

// GalaxyPeer.59E6304A passes the successful sign-in operation in ECX.
using FinishSignIn = void(__thiscall *)(void *operation) noexcept;
// Reads the borrowed context at operation+0x34 and returns it in EAX.
using OperationContext = void *(__thiscall *)(void *operation) noexcept;
// Receives the context in ECX and replaces its shared broker manager.
// Native startup requests friends, presence and chat subscriptions; later connection and delivery can fail.
using StartSessionBroker = void(__thiscall *)(void *context) noexcept;

} // namespace fc_plugins::galaxy_buddies
