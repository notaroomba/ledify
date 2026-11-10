# 11/09/2026 11 PM - Sound Modes, Beat Prediction and a New Panel

Now that a mic actually works I wanted the leds to react to sound properly. I looked at doing voice commands on the board first but ESP-SR's command recognition only runs on the S3/P4 and not the C6, so I did audio reactive modes instead. There are 5 new styles (eq, ripple, mirror, chroma and flow) that run at 60 fps, a `react` setting for how hard they respond, and the board now splits the sound into a 16 band spectrum. I also made a little simulator in the browser ([`web/public/sim.html`](web/public/sim.html)) that models the ICS-43434 from its datasheet so I can try the modes without the board plugged in

![image](assets/journal/sound_simulator.jpeg)

The problem with reacting to sound is that by the time the board hears a beat its already happened, so I added a tempo tracker ([`firmware/main/beat.c`](firmware/main/beat.c)) that predicts when the next beat is coming and lights it on time. In the sim the predicted beats land within a few ms of the hit instead of 10+ ms late. Its not perfect yet, around 160 BPM it locks onto half the tempo

Then I redid the whole web panel. Before it was a long page of cards, now its one screen with a live ring of the strip in the middle, a dock of lenses (colour, effects, gradient, sound, zones, time, looks and the board) and a command bar on Cmd-K

![image](assets/journal/panel_ring.jpeg)

![image](assets/journal/panel_phone.jpeg)

The colour, gradient and zones lenses also got their own brightness slider since the ring is busy picking hues or moving stops there. And since I have more than one board now you can rename a board from the Board lens, the name is also its mDNS name so a board called desk answers at desk.local

![image](assets/journal/board_rename.jpeg)

I also added the lesbian flag to the built in gradients as 7 hard stripes

![image](assets/journal/lesbian_gradient.jpeg)

None of the sound stuff has been flashed onto a board yet so thats next.

# 11/09/2026 8 PM - Second Board, Mic and Wi-Fi Fallbacks

So the mic on the first board never heard anything, it just sat flat around -67 dBFS no matter how loud it was. I reflowed it with hot air and it was the same, but then I tried the second board and its mic works!! So the footprint and the sound hole are fine and the first board's ICS-43434 is just damaged, probably from the two rounds of hot air, so it needs a new mic. I also added a `mic` command to the console that prints the level a few times a second which made testing way easier.

The second board also had a solder bridge on L2 which I cleared, and it doesnt get a PD contract from my mac so I still have to try it on a real PD charger. Another board has trouble with its 3V3 rail that I haven't figured out yet.

Then Wi-Fi. I take the board to different places so I added up to 5 fallback networks (`wifi2`), and it tries each saved network once in turn and loops back to the first one instead of retrying the same network over and over. I also found that the board crashed on boot if no Wi-Fi was saved at all (lwIP was never started) and that it could panic if a disconnect came in while it was restarting, so I fixed both.

And since I have more than one board now I wrote [`firmware/creds.py`](firmware/creds.py), which pulls a board's saved settings into a local creds.csv (which isnt in the repo) and pushes them onto another board so I dont have to type everything in again.

# 10/2026 - Stencil, Paste and First Light

The boards came in!! I got the stencil out and taped a board down in a jig made out of PCB rulers and kapton tape so it couldnt move

![image](assets/journal/stencil_jig.jpeg)

then clamped the stencil over it with some wood blocks

![image](assets/journal/stencil_clamped.jpeg)

and spread the paste across

![image](assets/journal/stencil_paste.jpeg)

I did three boards like this

![image](assets/journal/pasted_boards.jpeg)

and checked every one under the microscope with the layout open in KiCad next to it, mostly the ESP32-C6 since its 0.4mm pitch and the mic since its tiny

![image](assets/journal/microscope_bench.jpeg)

![image](assets/journal/microscope_bench_2.jpeg)

![image](assets/journal/microscope_paste.jpeg)

Heres an assembled board next to a bare one

![image](assets/journal/assembled_and_bare.jpeg)

![image](assets/journal/assembled_and_bare_2.jpeg)

and all three done

![image](assets/journal/three_boards.jpeg)

Then I wired one up to a strip on the VCC, SIG and GND pads and plugged in USB-C

![image](assets/journal/board_wired.jpeg)

and it lit up!!!

![image](assets/journal/first_light_red.jpeg)

![image](assets/journal/first_light_green.jpeg)

# 10/15/2026 - PD Debugging and a Lot of Firmware

So the board would sometimes die early in boot while it was negotiating PD, and I had no idea at which step. So I made the firmware keep a boot counter, the reset reason and the PD result in NVS, then a log of the last 4 boots, and then a log of every single handshake step as it happens. With that I made it fall back to a safe 5V boot after two early deaths, use the charger's 5V rating after a reject, and start PD 100 ms after boot instead of 2 s (with a `pddelay` setting to change it).

Then I went through the panel twice, first onto Tailwind and then I rewrote it on SolidJS and Vite with optimistic controls so sliders dont jump back while the board answers. On the firmware side I added:

- gradients with positioned stops (up to 16) and a droop setting that balances the warm far end of a strip thats only fed from one end
- transitions, sleep and sunrise timers and schedules
- zones
- a local panel served by the board itself over mDNS and WebSocket, and setup over Web Bluetooth
- DDP and E1.31 so other programs can stream pixels to it
- album cover colours as their own gradient, stripes with a run length per colour, and scrolling gradients and stripes
- a colombia gradient (with pure red because the flag red looks pink on leds lol)

I also wrote down what I want for rev 2 in the [README](README.md): current sensing, a second data output, a 5.2V setting for long strips, power injection pads, a user button, and checking the mic port, since the first board's mic doesnt hear anything.

# 9/30/2026 - Swapping the Mic and Final Files

_Time spent: 2.5h_

the i2s mic I picked (MSM261S4030H0R) was out of stock on LCSC so I spent a while looking for another one. apparently top port i2s mics basically don't exist, almost all of them have the hole on the bottom, so I went with the ICS-43434 which is a bit smaller and listens through a hole in the pcb

the footprint had the sound hole as a plated hole with no copper around it which jlc can't make so I changed it to a 0.5mm non plated hole

I also double checked the antenna again. the feed trace is the 50 ohm width for the JLC04161H-3313 stackup and nothing is inside the keepout on any layer so I just have to remember to pick that stackup when I order

then I fixed a 0.1mm track I missed, exported the production files again and redid the renders with the new parts

![image](assets/flyer.png)

with the extra sensors the pcba is around 300 usd now for 2 assembled boards, I looked at how much it would be to assemble them myself and its way cheaper per board but the esp32 is 0.4mm pitch and there are 3 LGA sensors so I'm going with pcba for this one

# 9/29/2026 - Light Sensor, Return Paths and a Mic

_Time spent: 4h_

so the board was "done" but I kept thinking of stuff to add lol

first I wanted an ambient light sensor so the leds can dim themselves when the room is dark. I went with the OPT4001 and it's i2c so I just put it on the same bus as the FUSB302 and didn't have to use any more pins. the only space left was next to the 3V3 buck so I put it on the output side away from the inductor. when I added it I accidentally ran some tracks straight through a group of 3V3 vias and shorted CHIP_EN and MISO to 3V3 so I had to fix that

then I looked at the return paths around the esp32 because I thought it was interesting and it turns out I had signal tracks on the inner layers cutting slots in the ground plane right under the chip, and there was only ONE ground via under the esp. so I moved CS_BARO and LED_CON off the planes, changed a few pins around (led data is on GPIO5 now and CS_BARO on GPIO7) and added more ground vias under it

after that I wanted a microphone so the leds can react to sound. I started with an analog one (ZTS6117) but the output is only a few millivolts so the esp adc can't really read it without an op amp and like 10 more parts. so I looked at the MAX9814 and then decided on an i2s mic instead because its one part and the signal is digital so the bucks can't mess with it
![image](assets/zms.png)
I also tied SYNC/MODE on the big buck to 3V3 so it always switches at the same frequency and doesn't make the caps hum next to the mic

# 9/28/2026 - Renders, Branding and Fixing Errors

_Time spent: 5h_

ookay so with the pcb done I started on the blender render yesterday and yea

I exported the board as a pcb3d and imported it into blender and then set up the camera and a bunch of lights around it

![image](assets/blender_setup.png)

the first renders looked kinda drab so I went through a few versions moving the lights around and giving them colors (red green and blue like an rgb led lol) until it looked good

then today I went on figma and made the logo and the banner and also a flyer that shows everything on the board. I also found a cool pixel font for the name and put it on the silkscreen too

![image](assets/figma_setup.png)

and this is how it ended up!!

![image](assets/ledify.png)

but then I went back and double checked the schematic and found a few errors lol

the biggest one was that the FUSB302 i2c lines weren't even connected to the esp (they just went to the pull up resistors) so it wouldn't have been able to get anything above 5V which is kinda the whole point of the board. I also had the boot button on GPIO8 instead of GPIO9 so I moved it and put a pull up on GPIO8

then theres the usb esd chip, I had its VBUS pin on VBUS but it can only handle like 6V so with PD at 20V it would have died, so now its on 3V3. I also added a pull down on the led enable so the buck doesn't turn on by itself when the esp is booting

after that I went through all of the LCSC part numbers and some of them didn't match, like the 10uF caps on VBUS were actually 25V instead of 50V and the crystal caps were wrong (the crystals want a 12pF load so the caps have to be 18pF) so I fixed those too

![image](assets/schematic_final.png)

![image](assets/pcb_final.png)

and now the drc has no more errors and the bom matches the schematic so its finally ready to order

# 9/27/2026 - JLCPCB Quote

_Time spent: 2.5h_

I then did a think with LCSC and JLCPCB and got the final quote on it, I had to bom match because of the 22uF caps adn they had to be rated for 25V because yea usb goes up to 20v
![image](assets/cart.png)

In total it went up to 210 usd ish with shipping and assembly so I think that this is good.

# 9/26/2026 - Routing and the Antenna

_Time spent: 8h_

![image](assets/usb_routing.png)

I routed the usb stuff first since that was the easiest and then went through the rest

![image](assets/routing_progress.png)

then it was time for the antenna and heres where I had to learn a bit more about rf theory. I'm using a meander pcb antenna and apparently the ground plane is literally part of the antenna so it has to be at a specific distance from it. I originally had a massive keepout below it with the feed trace running through empty board which is bad because the trace has no ground reference so its basically another antenna lol

so I pulled the ground pour up so its right under the antenna pads and made the feed trace super short with vias on both sides and yea

I also had to make a matching circuit for the first time!! the esp is like 35 + 10j ohms and the antenna wants 50 + 0j so I had to calculate the pi network to get it there and it was the first time actually doing the smith chart stuff for real instead of just copying values

![image](assets/lm61495_layout_datasheet.png)

for the big buck I followed the layout in the datasheet and kept the input caps right at the pins since thats the hot loop. I was also wondering if I should mirror the LED_VCC pour on the bottom with vias for the current but it would cut a hole in the ground plane so I left the ground plane alone

and after around 7 hours of routing today its DONE

![image](assets/finished_pcb.png)

![image](assets/render_3d.png)

# 9/20/2026 - Schematic Done and PCB Layout

_Time spent: 5h_

![image](assets/schematic_done.png)

schematic done so now its time for the pcb!!!

I had some trouble laying it out at first because the led inductor is HUGE (17.5mm) and I didn't know where to put it next to the esp and the flash. turns out its fine to have it next to the flash as long as the switch node is pointing away and the ground plane is solid so I just rotated it

the bigger problem was the antenna, I originally had it in the top left with the led pads and the crystal right next to it. so I moved the antenna to the top right corner against both edges and the led pads to the other side and the crystal below the esp

after a bit of moving stuff around I finally decided on a general layout and grouped everything (usb, small buck, big buck, esp + flash) and started routing

# 9/19/2026 - 5V/12V Switching and a Pull Down

_Time spent: 6h_

after that I had to go back and look at the datasheet for the big buck again because I wanted to make sure the 5V/12V thing actually worked. basically theres a voltage divider on the feedback pin and the 2N7002K puts another resistor in parallel with the bottom one so off = 5V and on = 12V

but then I realized that when the esp is booting the gpio floats so the mosfet could randomly turn on and send 12V to a 5V strip and kill it so I decided to put a pull down resistor on the gate so it defaults to 5V

![image](assets/pulldown.png)

also had to double check all the caps because VBUS can go up to 20V with PD so the input caps need to be rated for way more than what I had

# 9/13/2026 - ESP32-C6 Pinout and Schematic

_Time spent: 6h_

ookay so I've been working on this on and off whenever I had time and I finally got around to finishing the schematic

the thing was I didn't know the esp32 pinout at all so I went through the datasheet and looked at some example schematics that other people made to get an idea of what goes where

![image](assets/esp32c6_reference_schematic.png)

![image](assets/example_esp32c6_schematic.png)

and thats when I found out that on the esp32 literally ANY gpio can be spi or i2c because of the gpio matrix (with some regulations lol) so like you cant use 24-30 because thats the flash on the 40 pin version and 12/13 are usb and 8/9/15 are strapping pins so nothing can pull them at boot. also there are no input only pins on the c6 unlike the og esp32 so that made it easier and yea

# 9/12/2026 - Everything but the ESP32

_Time spent: 7h_

got basically everything done except the esp32 itself

so I have the usb c going into the FUSB302 for the PD stuff over i2c and then a USBLC6 for esd on the data lines. then theres the small buck (AP63203) that makes the 3V3 for the esp and the big buck with the HUGE inductor for the leds and yea I have the mosfet thing on the feedback so I can switch 5V or 12V from software

also added the crystal and all the decoupling caps for the esp, theres like a million power pins on the c6 (VDDA, VDDA3P3, VDDPST, VDD_SPI) so I put a ferrite on the analog one and caps on all of them like the datasheet says

![image](assets/everything_but_esp.png)

now I just have to figure out the esp pinout because I have no idea what goes where lol

# 9/5/2026 - Starting Out and the Bucks

_Time spent: 6h_

So I'm doing a thing where I'm trying to make a simple led controller that works with USB-C PD and yea. I want this to be able to power either 12V or 5V LEDs depending on a switch in software and ya. I got this so far as I'm using the FUSB302BMPX for USB-C PD negotiation and yea. I still have to choose my buck converters.

Finished big buck and also small buck and learned about n channel and p channel mosfets.
![image](assets/buck.png)
