# AEGEON — Vertical Slice Prototype Brief

> **Status:** Prototype concept  
> **Working title:** AEGEON  
> **Genre:** Calm, stylized 3D island strategy game  
> **Platform:** PC  
> **Prototype focus:** Peaceful island civilization building  
> **Visual direction:** Polished stylized 3D inspired by the warmth and readability of classic browser strategy games, with an original Ancient Mediterranean setting

---

## 1. High Concept

AEGEON is a calm, approachable 3D strategy game about founding and growing a small Ancient Mediterranean island settlement. The player gathers resources, develops a compact town, supports its population, and trades with a nearby island.

The vertical slice should prove that **building a small, attractive, economically functional island settlement is satisfying on its own**. It is not intended to represent the eventual full game, which may balance economy, diplomacy, exploration, and warfare more equally.

The prototype should feel peaceful and readable rather than idle: the player makes meaningful decisions about what to build, what to produce, and when to trade.

## 2. Prototype Goals

The vertical slice should answer these questions:

1. Is placing buildings and watching a settlement grow enjoyable?
2. Can the player understand the resource economy without excessive menus?
3. Does the island feel alive through small character and environmental animations?
4. Does a short trade-and-growth objective provide a satisfying beginning, middle, and end?
5. Can the experience support a future PC-focused strategy game with more systems?

### Success criteria

The slice is successful when a new player can, without external instructions:

- Understand the basic resource HUD.
- Place at least three building types.
- Maintain a population by supplying basic needs.
- Produce a surplus resource.
- Complete a trade with a neighboring island.
- Reach a clear prototype completion state in approximately 15–25 minutes.

## 3. Core Player Experience

**Fantasy:** “I am nurturing a prosperous Mediterranean island community.”

The player should feel:

- Relaxed, but not without decisions.
- Proud of the town's visible growth.
- Curious about nearby islands and their goods.
- In control of the settlement's priorities.
- Rewarded by clear, tangible feedback.

Avoid timers and penalties that demand constant attention. Production can continue while the player plans, but the prototype should not require offline progression.

## 4. Core Gameplay Loop

1. **Gather** starting resources and inspect the island.
2. **Plan** the settlement layout and place essential buildings.
3. **Produce** food and construction materials.
4. **Grow** the population when its basic needs are met.
5. **Specialize** in one export good.
6. **Trade** with a neighboring island for a needed resource.
7. **Improve** the settlement with a civic building or upgraded home.
8. **Complete** the prosperity objective.

The loop should be short, legible, and repeatable, with the trade objective providing a reason to build an economy rather than simply placing every building.

## 5. Scope of the Vertical Slice

### Included

- One small, hand-authored Mediterranean island.
- One player-controlled settlement.
- One neighboring island represented by a ship route, map marker, or simple port screen.
- A compact building placement system.
- A small resource economy.
- Population and basic needs.
- Passive production with clear rates.
- A simple trade interface.
- A short tutorial or contextual prompts.
- A prosperity goal and completion screen.
- Stylized character and environmental animation.

### Explicitly excluded

- Combat and military unit control.
- Diplomacy beyond a simple trade relationship.
- Multiple playable civilizations.
- Procedural world generation.
- Large-scale naval simulation.
- Multiplayer.
- Complex logistics, road traffic, or worker pathfinding.
- Deep technology trees.
- Random disasters and punitive event systems.
- Full campaign, narrative, or endgame progression.

These exclusions are deliberate. This slice tests the peaceful settlement-building pillar first; they do not define the scope of the eventual full game.

## 6. Setting and Art Direction

### World

A fictional Ancient Mediterranean archipelago: warm coastal light, turquoise and deep-blue water, rocky coves, olive groves, terraced hills, pale limestone, clay roof tiles, and distant islands.

Use historical Mediterranean influences without claiming strict historical simulation. The world should be coherent and original rather than a direct copy of any existing game's setting or interface.

### Visual principles

- **Polished stylized 3D:** simplified, appealing forms with carefully controlled detail.
- **Readable silhouettes:** buildings and characters should be identifiable from the normal gameplay camera.
- **Warm materials:** limestone, terracotta, wood, linen, bronze, and painted ceramics.
- **Calm lighting:** bright daylight, soft shadows, gentle water movement, and restrained bloom.
- **Friendly characters:** expressive faces and slightly exaggerated proportions.
- **Useful animation:** citizens carry goods, traders gesture, and ships bob gently at the harbor.
- **Low visual noise:** UI and scenery must not compete with strategic information.

### Starter palette

| Use | Color direction |
|---|---|
| Sea | Turquoise, Mediterranean blue, deep navy |
| Stone | Warm ivory, pale limestone, sand |
| Roofs | Terracotta, muted coral |
| Vegetation | Olive green, sage, cypress green |
| Civic accents | Aegean blue, aged bronze |
| UI | Warm parchment, dark marine blue, restrained gold |

## 7. Prototype Characters

Characters should add warmth and communicate game systems. In this slice, they are presentation and tutorial devices, not deep RPG heroes.

### The Merchant

- **Role:** Introduces trade and resource exchange.
- **Appearance:** Friendly, slightly stocky adult; cream tunic, blue drape, leather satchel, coin purse.
- **Behavior:** Greets the player at the port, gestures toward goods, reacts positively to a successful trade.
- **Gameplay function:** Provides a short explanation of export goods and trade offers.

### The Citizen

- **Role:** Represents population and daily life.
- **Appearance:** Approachable adult; cream linen clothing, blue sash, pottery or carried goods.
- **Behavior:** Walks between a home, workplace, and storage/market area using simple looping paths; idles and reacts to prosperity.
- **Gameplay function:** Makes population growth and shortages visible.

### Presentation-only characters

If time allows, add one stationary soldier or guard near the settlement entrance and one archer as a decorative harbor or wall presence. They do not participate in combat in this prototype. Do not build military mechanics for the peaceful slice.

## 8. Buildings

Keep the building roster deliberately small. Every building must support the core loop.

| Building | Purpose | Prototype behavior |
|---|---|---|
| Town Hall | Central settlement landmark and objective tracker | Displays population and prosperity state |
| House | Supports population | Adds residents when food and housing conditions are met |
| Farm | Produces food | Adds food at a fixed interval |
| Woodcutter's Camp | Produces timber | Adds timber at a fixed interval |
| Quarry | Produces stone | Adds stone at a fixed interval |
| Workshop | Converts timber into pottery or another export good | Consumes timber and produces trade goods |
| Market/Storehouse | Shows local stock and enables trading | Central resource and trade interface |
| Harbor | Opens the trade interface | Starts and resolves a simple ship trade |

**Minimum viable building set:** Town Hall, House, Farm, Woodcutter's Camp, Market/Storehouse, and Harbor. Add the Quarry and Workshop only if the core loop is already stable.

## 9. Resources and Economy

Start with no more than five resource types.

| Resource | Main source | Main use |
|---|---|---|
| Food | Farm | Supports population |
| Timber | Woodcutter's Camp | Construction and crafting |
| Stone | Quarry or starting stock | Selected construction costs |
| Pottery | Workshop | Export good for trade |
| Coins | Trade reward | Optional construction or upgrade costs |

### Economy rules

- Production is passive and shown as a clear rate per minute.
- A building only produces if its input requirements are satisfied.
- Food consumption scales with population in a simple, predictable way.
- The player receives a small starting stock so the first few actions are immediate.
- Trade prices are fixed in the vertical slice; no dynamic market simulation.
- Shortages should be communicated clearly before they become severe.
- Avoid hidden multipliers and opaque production chains.

Example prototype rates can be tuned during playtesting. The goal is to test the loop, not to simulate a historically accurate economy.

## 10. Population and Prosperity

Population is an easy-to-understand measure of settlement growth.

- Houses define housing capacity.
- Available food supports population growth.
- A simple happiness/prosperity indicator reflects whether basic needs are met.
- If food is insufficient, growth pauses and the UI explains why.
- Avoid sudden population loss in the vertical slice; the prototype should teach through feedback rather than harsh punishment.

### Prototype objective

Reach a target such as:

- 30 residents.
- At least one export trade completed.
- A functioning food supply.
- A Market/Storehouse and Harbor built.

The exact values should be tuned to fit a 15–25 minute first playthrough.

## 11. Trade

Trade is the slice's main strategic choice.

- The player selects a small quantity of pottery or another export good.
- The neighboring island offers one needed resource in exchange.
- A ship travels along a visible, short route or the UI shows a compact departure/arrival sequence.
- On completion, the player receives the agreed goods and a clear notification.
- Trade should be a deliberate exchange, not a one-click reward with no economic consequence.

For the first implementation, use one neighboring island and one fixed trade offer. The ship animation can be cosmetic; a full naval pathfinding system is out of scope.

## 12. Camera and Controls

Designed for PC with mouse and keyboard.

- Isometric or elevated three-quarter camera.
- Edge scrolling or WASD camera movement, with configurable sensitivity.
- Mouse-wheel zoom across a useful range.
- Middle-mouse drag to pan is a suitable alternative to edge scrolling.
- Left-click selects buildings and UI targets.
- Building placement uses a clear ghost preview and valid/invalid placement feedback.
- Escape closes panels and cancels placement.
- Avoid requiring precise, small targets.

A modest camera tilt and restrained depth of field should preserve readability. Do not use cinematic depth of field during normal strategic play.

## 13. UI and Feedback

### Persistent HUD

- Resource counts and production/consumption rates.
- Population and housing.
- Prosperity or basic-needs status.
- A compact objective tracker.
- Pause/settings access.

### Contextual panels

- Building placement and cost.
- Selected building's production and status.
- Market inventory and trade offer.
- Harbor trade status.
- Completion summary.

### Feedback principles

- Show why an action is unavailable.
- Use clear icons plus text for resource types.
- Animate resource gains subtly.
- Use short, non-intrusive notifications.
- Keep the visual hierarchy stable as the town grows.
- Do not make essential information dependent on color alone.

## 14. Audio and Atmosphere

- Gentle sea ambience, birds, distant harbor sounds, and soft wind.
- Light, warm Mediterranean-inspired instrumental music.
- Small UI sounds for placement, trade completion, and milestone progress.
- Subtle building and citizen sounds that do not become repetitive.
- A simple volume mixer and mute options.

The audio should support focus and relaxation, not create urgency.

## 15. Suggested Vertical Slice Milestones

### Milestone 1 — Playable Island

- Camera movement and zoom.
- One island environment.
- Selectable Town Hall.
- Building placement with valid/invalid preview.
- Initial resource HUD.

### Milestone 2 — Functional Economy

- Farm and Woodcutter's Camp production.
- Resource storage and construction costs.
- Houses and population capacity.
- Food consumption and growth rules.

### Milestone 3 — Trade and Objective

- Harbor and market interface.
- One neighboring island and one fixed trade offer.
- Ship departure/arrival feedback.
- Prosperity objective and completion state.

### Milestone 4 — Character and Art Pass

- Merchant and Citizen presentation.
- Ambient character animation.
- Water, vegetation, lighting, and environmental polish.
- Basic soundscape and UI feedback.

### Milestone 5 — Playtest and Polish

- Test with a player unfamiliar with the prototype.
- Observe whether the first building, production chain, and trade are understandable.
- Remove unnecessary steps and confusing UI.
- Tune the objective to a 15–25 minute playthrough.
- Record remaining technical and design risks.

## 16. Minimum Acceptance Checklist

- [ ] The player can move and zoom the camera.
- [ ] The player can place, select, and inspect buildings.
- [ ] Resources are earned and spent consistently.
- [ ] Production rates and shortages are visible.
- [ ] Houses support population growth.
- [ ] Food supply affects growth predictably.
- [ ] The player can complete a trade with the neighboring island.
- [ ] The objective tracker communicates progress.
- [ ] The prototype has a clear completion state.
- [ ] The visual and audio presentation feels calm and cohesive.
- [ ] The slice can be completed in approximately 15–25 minutes.
- [ ] No excluded system is required to complete the slice.

## 17. Risks and Guardrails

- **Scope creep:** Do not add combat, multiple islands, or advanced diplomacy before the slice is playable end-to-end.
- **Too much passive waiting:** Ensure the player has meaningful planning and placement decisions while production runs.
- **Unreadable visuals:** Test the camera at normal gameplay zoom, not just close-up art views.
- **Economy complexity:** Start with fixed rates and fixed trade prices; add complexity only when it improves decisions.
- **Decorative characters without purpose:** Give each visible character a clear role in communicating population, trade, or settlement activity.
- **Overly demanding pacing:** No constant alerts, forced check-ins, or punishing time pressure.

## 18. Future Full-Game Direction — Deliberately Out of Scope

The eventual game is intended to be a **balanced island empire-building RTS**, with economy, diplomacy, exploration, and warfare all important. That broader direction is not implemented in this vertical slice.

The slice should create reusable foundations where practical—resource definitions, building data, selection, camera controls, and UI patterns—but avoid speculative frameworks for systems that have not been designed.

After the slice is validated, the full game can be planned separately around:

- Multi-island expansion and world map.
- More complex production chains and logistics.
- Diplomacy, alliances, treaties, and rival civilizations.
- Land and naval warfare.
- Military unit selection, formations, and command feedback.
- Exploration, discoveries, and contested resources.
- Progression, research, and long-term objectives.
- AI opponents and difficulty options.

## 19. Prototype Definition of Done

The prototype is done when a player can start on a small island, establish food and timber production, grow a modest population, create an export good, complete a trade, and reach the prosperity goal—with clear feedback and without needing developer guidance.

**Guiding principle:** Prove the pleasure of building a living island settlement before expanding into the larger empire strategy game.
