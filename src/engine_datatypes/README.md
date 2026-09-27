# engine_datatypes

Values a property holds and a script passes around: `Vector3` (`Vec3` in C++), `Color` (`ColorRgb`), and `Transform`. A value has no identity and no place in the tree. `types.hpp` in engine_core includes these headers, so most code gets them without asking.

`Vector3.cpp` also installs the `Vector3` library into a Luau state. `Enum.cpp` installs `Enum`: `NormalId`, `Axis`, and the input enums `KeyCode`, `UserInputType`, and `UserInputState`, with Roblox's values. Each item is one userdata, so `==` holds, and a function that takes an enum accepts the item, its name, or its value.
