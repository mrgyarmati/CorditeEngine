# Input names

The names an input map binds controls by. Written by the engine (`inputReference`, editor/ReferenceDocs.h) and held to it by a test: do not change it by hand. How an input map is written is in the [guide](../guide/input.md).

## Keys

For `key`, `positiveKey` and `negativeKey`. Keys are named by where they are on a US keyboard, whatever the layout of the keyboard says on them.

`Space`, `Apostrophe`, `Comma`, `Minus`, `Period`, `Slash`, `Num0`, `Num1`, `Num2`, `Num3`, `Num4`, `Num5`, `Num6`, `Num7`, `Num8`, `Num9`, `Semicolon`, `Equal`, `A`, `B`, `C`, `D`, `E`, `F`, `G`, `H`, `I`, `J`, `K`, `L`, `M`, `N`, `O`, `P`, `Q`, `R`, `S`, `T`, `U`, `V`, `W`, `X`, `Y`, `Z`, `LeftBracket`, `Backslash`, `RightBracket`, `GraveAccent`, `Escape`, `Enter`, `Tab`, `Backspace`, `Insert`, `Delete`, `Right`, `Left`, `Down`, `Up`, `PageUp`, `PageDown`, `Home`, `End`, `CapsLock`, `ScrollLock`, `NumLock`, `PrintScreen`, `Pause`, `F1`, `F2`, `F3`, `F4`, `F5`, `F6`, `F7`, `F8`, `F9`, `F10`, `F11`, `F12`, `Keypad0`, `Keypad1`, `Keypad2`, `Keypad3`, `Keypad4`, `Keypad5`, `Keypad6`, `Keypad7`, `Keypad8`, `Keypad9`, `KeypadDecimal`, `KeypadDivide`, `KeypadMultiply`, `KeypadSubtract`, `KeypadAdd`, `KeypadEnter`, `KeypadEqual`, `LeftShift`, `LeftControl`, `LeftAlt`, `LeftSuper`, `RightShift`, `RightControl`, `RightAlt`, `RightSuper`, `Menu`

## Mouse buttons

For `mouseButton`.

`Left`, `Right`, `Middle`, `Button4`, `Button5`

## Gamepad buttons

For `gamepadButton`, `positiveButton` and `negativeButton`.

`A`, `B`, `X`, `Y`, `LeftBumper`, `RightBumper`, `Back`, `Start`, `Guide`, `LeftThumb`, `RightThumb`, `DpadUp`, `DpadRight`, `DpadDown`, `DpadLeft`

## Gamepad sticks and triggers

For `gamepadAxis`.

`LeftX`, `LeftY`, `RightX`, `RightY`, `LeftTrigger`, `RightTrigger`

## The pointer and the wheel

For `mouse` and `scroll`: `x` and `y`.
