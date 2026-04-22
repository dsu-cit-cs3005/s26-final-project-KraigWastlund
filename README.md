This is the starter repo for the RobotWarz assignment.

Included files:

- `RobotBase.h` and `RobotBase.cpp`: Base robot API used by all arenas. Do not modify.
- `RadarObj.h`: Radar result object shared between robots and arena. Do not modify.
- Sample robots: `Robot_Ratboy.cpp`, `Robot_Flame_e_o.cpp`
- `test_robot.cpp`: Local robot contract tester.
- `Makefile`: Builds `test_robot` and `RobotBase.o`.
- Design/spec docs: `.drawio` files and `RobotWarz_spec.md`.

## What You Must Build

1. Your own Arena implementation (recommended: `Arena.h` + `Arena.cpp`).
2. A `RobotWarz` executable (`./RobotWarz`) that runs your turn-based simulation.
3. At least one custom robot file named `Robot_<Name>.cpp`.

## Files You Must Not Change

- `RobotBase.h`
- `RobotBase.cpp`
- `RadarObj.h`

## Robot Contract (Required)

Each robot must derive from `RobotBase` and implement all required virtual functions.

Each robot source must export BOTH symbols:

```cpp
extern "C" RobotBase* create_robot();
extern "C" const char* robot_summary();
```

`robot_summary()` rules:

- Required for grading compatibility.
- Must return non-null text.
- Max length: **50 characters**.
- Should briefly describe behavior strategy (example: "Hugs wall, railguns nearest target.").

## Turn Rules

A robot may perform exactly one action per turn:

- Shoot OR
- Move OR
- Do nothing

Robots should not rely on both shooting and movement in the same turn.

## Portability Rules (Important for Grading)

- Keep robot files self-contained and portable.
- Do not add external dependencies that won't exist in the grading repo.
- Assume your robot will be compiled and loaded into a different arena.

## Local Testing

Build tester:

```bash
make
```

Test a robot file:

```bash
./test_robot Robot_Ratboy.cpp
```

The tester checks compile/load behavior, required exports, summary length, and basic turn behavior.

## Xcode Setup

This starter now includes an Xcode project:

- `RobotWarzStarter.xcodeproj`
- scheme: `test_robot`

How to use:

1. Open `RobotWarzStarter.xcodeproj` in Xcode.
2. Select the `test_robot` scheme.
3. Build/Run (`Cmd+B`, `Cmd+R`).

Default run argument is `Robot_Ratboy.cpp`.

As you build your arena, you can add your own `RobotWarz.cpp`/`Arena.cpp` files to the project
and create a `RobotWarz` executable target in Xcode.

## Notes

- Build a simple working arena first, then iterate.
- Keep arena logic modular (loading, movement, shooting, radar, rendering, win condition).
- If you think starter files have a bug, create a GitHub issue for the instructor.
