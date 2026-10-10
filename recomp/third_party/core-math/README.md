Correctly rounded binary64 tan, sin, cos and atan2 from the CORE-MATH project (https://core-math.gitlabpages.inria.fr/,
https://gitlab.inria.fr/core-math/core-math, src/binary64/{tan,sin,cos,atan2}, fetched 2026-10-09). MIT licence: see the
header of each file (Copyright (c) 2022-2025 Paul Zimmermann, Tom Hubrecht and the CORE-MATH authors).

The runtime's x87 fptan, fsin, fcos and fpatan use these instead of the host libm, so every platform gets the same,
correctly rounded result (Apple's libm differed from glibc in the last bit for 96% of Rainbow Six's tangents, enough to
move the spectator camera across a doorway plane and blank a frame). glibc agrees with them for every tangent the game
computed and all but ~0.03% of its sines, cosines and arctangents.
