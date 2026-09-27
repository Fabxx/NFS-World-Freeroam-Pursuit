#pragma once

// EVADED/BUSTED results screen for our pursuit.
// The PostRace Flash screen asks native HandleRequest_Interop_ArbitratedPacket
// (sub_51E610) for the server-arbitrated packet. GameCore has none for our
// pursuit and would throw (NullReferenceException -> EASharp int3 = crash),
// so while Pursuit::InResultsWindow() the request is answered here, the way
// GameCore does it: request object + native PursuitArbitratedPacket filled
// with our stats, handed to sub_51B0E0 synchronously (no callback name ->
// the packet is the return value of the ExternalInterface call).
// Player/car fields (name, car, HAT ratings...) come from the entrant of the
// last REAL pursuit results screen, captured when the game serializes it and
// kept in NFSWorldPursuitProbe_entrant.txt next to the .asi.
namespace Mod::Results {
    void Install();
    void Remove();
}
