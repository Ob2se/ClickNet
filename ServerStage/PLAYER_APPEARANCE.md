# Player appearance setup

Use one `UPlayerAppearanceCatalog` Data Asset, with an `AppearanceClasses` map:

- Key `0`: your default `BP_Box3DPawn` class.
- Key `1`: an alternate pawn Blueprint class, if needed.

The subsystem loads `/Game/DA_PlayerAppearanceCatalog.DA_PlayerAppearanceCatalog`
at startup using `AppearanceCatalogPath` in `Config/DefaultGame.ini`. Change that
path if you rename the asset. The catalog stores `APawn` classes so the plugin
does not depend on the project's `ClickNet` module. The assigned Blueprint can
still inherit from `ABox3DPawn`.

Call `SetPlayerAppearance(0)` or `SetPlayerAppearance(1)` on the ClickNet subsystem.
It can be called before connecting or while connected. The server currently
accepts IDs 0 and 1; extend `MaxAppearanceId` in `ClicknetServer.h` when adding IDs.
This selects the appearance announced to other clients; it does not replace the
locally possessed pawn.

The server sends the player's ID, appearance ID and server tick reliably when a
player enters visibility or changes appearance. The client pairs appearance with
movement state and spawns at most two remote pawns per frame. Remote pawns use the
existing collision-constrained smoothed position and yaw. They do not possess a
controller, send movement input, or simulate local physics. They are destroyed on
despawn or disconnect. This adds visual models; player blocking remains a separate
server movement feature.

For packaged builds, ensure the catalog asset and its referenced Blueprint classes
are included in the cook (for example through a Primary Asset Label with Cook Rule
Always Cook). A config path alone does not guarantee asset cooking.

Wire version is now 7. Rebuild the client, server and headless bots together.

## Animation movement state

`ABox3DPawn` implements the plugin's `IClickNetRemoteMovementReceiver` interface.
Each remote visual update forwards the existing replicated grounded flag and
gravity-predicted velocity to its disabled Box3D movement component. Animation
Blueprints can use `Box3DGetVelocity()` and `Box3DIsFalling()` for both local and
remote pawns. The velocity getter already returns centimeters per second; do not
multiply its result by 100 again. No new packets or animation replication are
required. Grounded remains authoritative; extrapolating velocity does not invent
landing events. Local movement simulation is unaffected by the remote setter.

## Changes

- Removed the reflected reference to the game module's `ABox3DPawn` from the
  plugin catalog; this fixes the undefined `CLICKNET_API` UHT compile error.
- Made Engine and CoreUObject public dependencies because plugin headers expose
  their types.
- Included player identity and tick in appearance packets, preventing an ID from
  being applied to an unidentified player.
- Loaded the existing catalog asset and connected appearance decoding to spawning,
  updating, replacing and removing remote pawn bindings.
- Initialized server appearance versions to 1 so default appearance 0 is sent.
- Disabled movement and physics for remote visual pawns to avoid competing with
  server replication.
