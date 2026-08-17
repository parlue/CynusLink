// CynusLink ESP32-S3 gateway enclosure V3
// ESP32-S3 DevKitC-1 style board
// Screwless two-piece snap fit, enlarged internal clearance.
// Units: mm

$fn = 48;
part = "base"; // "base", "lid", or "preview"

// PCB reference dimensions
pcb_l = 62.8;
pcb_w = 25.4;
pcb_t = 1.6;

// V3: deliberately more room than the earlier enclosure.
// Internal cavity is 66.5 x 30.5 mm.
inner_l = 66.5;
inner_w = 30.5;
wall = 2.0;
floor_t = 2.0;
base_h = 14.0;
corner_r = 2.8;

outer_l = inner_l + 2*wall;
outer_w = inner_w + 2*wall;

lid_t = 2.2;
lid_skirt_h = 4.0;
lid_clear = 0.30;

module rounded_box(l,w,h,r){
    hull(){
        for(x=[r,l-r], y=[r,w-r])
            translate([x,y,0]) cylinder(r=r,h=h);
    }
}

module base_shell(){
    difference(){
        rounded_box(outer_l, outer_w, base_h, corner_r);

        // Open cavity.
        translate([wall,wall,floor_t])
            rounded_box(inner_l, inner_w, base_h, max(corner_r-wall,0.7));

        // Wide end opening for the DevKitC-1 USB connector area.
        // Extra width/height gives clone boards some tolerance.
        translate([-0.2, outer_w/2-12.8, 3.8])
            cube([wall+0.7,25.6,7.5]);

        // BOOT / RESET access slots.
        translate([outer_l-19.0,-0.2,5.0]) cube([11.0,wall+0.5,4.5]);
        translate([outer_l-19.0,outer_w-wall-0.3,5.0]) cube([11.0,wall+0.5,4.5]);

        // Four snap pockets, INSIDE the long walls.
        for(x=[outer_l*0.31, outer_l*0.69]){
            translate([x-2.2, wall-0.05, base_h-3.5])
                cube([4.4,1.25,1.40]);
            translate([x-2.2, outer_w-wall-1.20, base_h-3.5])
                cube([4.4,1.25,1.40]);
        }
    }

    // PCB edge rails. Their position is based on the PCB width rather than
    // the enlarged cavity width, so the wider enclosure cannot let the board fall through.
    rail_z = 3.0;
    rail_h = 1.5;
    rail_w = 2.2;
    overlap = 0.9;
    y_low = outer_w/2 - pcb_w/2 - overlap;
    y_high = outer_w/2 + pcb_w/2 - rail_w + overlap;

    translate([wall+2.0, y_low, rail_z])
        cube([inner_l-4.0, rail_w, rail_h]);
    translate([wall+2.0, y_high, rail_z])
        cube([inner_l-4.0, rail_w, rail_h]);

    // Small retainers above the PCB edge to reduce rattling.
    ret_z = rail_z + pcb_t + 1.0;
    for(x=[wall+7.0, outer_l-wall-9.0]){
        translate([x, y_low+0.15, ret_z]) cube([2.4,1.2,1.2]);
        translate([x, y_high+rail_w-1.35, ret_z]) cube([2.4,1.2,1.2]);
    }
}

module lid(){
    difference(){
        union(){
            // Top plate.
            rounded_box(outer_l, outer_w, lid_t, corner_r);

            // Internal locating skirt / lip. Interior is +Z.
            translate([wall-lid_clear, wall-lid_clear, lid_t])
            difference(){
                rounded_box(
                    inner_l+2*lid_clear,
                    inner_w+2*lid_clear,
                    lid_skirt_h,
                    max(corner_r-wall+0.35,0.9)
                );
                translate([1.30,1.30,-0.1])
                    rounded_box(
                        inner_l+2*lid_clear-2.60,
                        inner_w+2*lid_clear-2.60,
                        lid_skirt_h+0.2,
                        0.7
                    );
            }

            // Snap bumps that mate with the internal pockets in the base.
            for(x=[outer_l*0.31, outer_l*0.69]){
                translate([x-1.6, wall-lid_clear-0.42, lid_t+1.15])
                    cube([3.2,0.9,0.85]);
                translate([x-1.6, outer_w-wall+lid_clear-0.48, lid_t+1.15])
                    cube([3.2,0.9,0.85]);
            }
        }

        // Ventilation slots.
        for(i=[0:5])
            translate([outer_l-30+i*3.4, outer_w/2-7.0, -0.2])
                rounded_box(1.55,14.0,lid_t+0.4,0.6);

        // V3: engraved label on the OUTSIDE of the lid.
        // Exterior is the -Z face. Mirroring in X makes it readable when
        // looking at the exterior surface.
        translate([outer_l/2, outer_w/2, -0.05])
            linear_extrude(height=0.70)
                mirror([1,0,0])
                    text(
                        "CYNUS LINK",
                        size=5.4,
                        font="DejaVu Sans:style=Bold",
                        halign="center",
                        valign="center"
                    );
    }
}

if(part=="base") base_shell();
else if(part=="lid") lid();
else {
    color("gray") base_shell();
    translate([0,0,base_h+0.6]) color("lightgray") lid();
}
