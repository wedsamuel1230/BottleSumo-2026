# BottleSumo domain glossary

## Chassis envelope

The maximum physical boundary of the assembled robot used for competition
inspection. It includes the body, sensor brackets, battery, PCB, motors, and
wheels in their normal operating configuration.

## Sensor head

One VL53L0X-class time-of-flight sensing module mounted as a directional unit.
The current baseline contains three heads: front-left, front-center, and
front-right.

## Parametric insert hole

A chassis mounting hole whose diameter is driven by a design parameter rather
than a fixed sketch value, allowing the same assembly to accept an M3 screw
path, a 4.2 mm heat-set insert, or a 5.0 mm heat-set insert after the selected
insert manufacturer's fit requirement is confirmed.

## PCB mounting datum

The reference geometry used to locate the 100 x 90 mm BottleSumo PCB and its
mounting holes relative to the chassis. The hole centers are 5 mm from the
corresponding PCB edges on all four corners.

## Battery envelope

The reserved mechanical volume and mounting interface for the WheelTech E326S
battery. The supplied E326S CAD model is the initial dimensional authority;
physical fit, connector clearance, and strap tension remain verification gates.

## FDM chassis material

The chassis is intended for fused-deposition modeling in PLA with 3 mm nominal
walls. Strength, layer orientation, and local reinforcement remain part of the
print validation rather than assumptions about injection-molded strength.

## One-piece chassis

The primary chassis is a single FDM-printed PLA body. The PCB is not enclosed
by a printed upper deck; it is elevated above the body on separate M3 brass
hex standoffs (六角铜柱) to preserve access for wiring and service.

## Opponent-facing ToF orientation

For this assembly, “90-degree VL53L0X rotation” means a -90 degree X-axis
rotation in Fusion's right-handed coordinate system. That sign maps the imported
sensor's local optical +Z normal to world +Y, the marked opponent-facing side.
The sign is part of the assembly vocabulary because a visually similar +90
degree turn points the heads toward -Y instead.

## Ground-facing QRE orientation

For this assembly, “180-degree QRE1113 rotation” means a 180 degree X-axis
rotation that maps the optical face to world -Z. The board is retained above a
full-board floor window so the optical face remains about 2.5 mm above the table
while the PCB and rail structure stay in the one-piece chassis body.

## Hardware capture rail

A printed lip or side wall that locates an imported sensor board by its measured
envelope while leaving its optical face and wiring access clear.

## Sensor fit gate

The mechanical proof that an imported board is seated, retained, and optically
clear in its chassis interface; a correct rotation matrix alone is not enough.
