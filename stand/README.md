# CrowPanel 7-inch acrylic-case feet

`crowpanel_dual_feet_14mm.stl` contains two identical, disconnected feet laid
out for a single print. STL units are millimeters.

- Nominal measured case thickness: 14.00 mm
- Printed slot width: 14.60 mm (0.60 mm total fit clearance)
- Screen lean: 15 degrees backward from vertical
- Each foot: 32 mm wide x 82 mm deep x 52 mm high
- Base: 8 mm thick
- Front retaining wall: 10 mm above the slot floor, leaving clearance for a
  screen whose visible area begins about 14 mm above the case bottom
- Rear support wall: 44 mm above the base
- Top contact edges: 1.5 mm chamfer

Print in the supplied orientation with the large flat bases on the build plate.
No supports are required. A practical starting profile is 0.20 mm layers, four
walls, four top/bottom layers, and 20-30% gyroid or grid infill in PLA or PETG.
Add thin felt, foam tape, or TPU inside the slots after checking the fit; this
protects the acrylic and removes any remaining play.

The feet are intentionally independent, so their spacing can match the acrylic
case without knowing its final width. If the delivered case differs from the
estimated thickness, edit the constants near the top of
`scripts/build_crowpanel_feet.py` and regenerate:

```bash
python3 scripts/build_crowpanel_feet.py
```
