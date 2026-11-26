// The settings menu, for the Sims 4 VR debug menu and this mod's own menu (esc_menu(), the format is
// jiggle/docs/MENU-EXPORT.md). One row per line, fields separated by TABs: keep the TABs when editing.
LR"ESC(page	main		Extended Sound Control
number	main	Master boost	boost	1	4	0.05	100	%.0f%%		Everything louder than the game allows, through its own limiter (no crackle).
number	main	Music boost	music_boost	1	4	0.05	100	%.0f%%		Radios, stereos and the score. On top of Master boost.
number	main	Sound effects boost	effects_boost	1	4	0.05	100	%.0f%%		On top of Master boost.
number	main	Voices boost	voices_boost	1	4	0.05	100	%.0f%%		Sims' voices. On top of Master boost.
number	main	Ambient boost	ambient_boost	1	4	0.05	100	%.0f%%		Birds, wind, rain, city hum (the game has no slider). Pick the group under Advanced settings first.
switch	main	Clarity	clarity		Stops the game muffling sounds below the Muffle floor and opens its treble cuts. Phone, TV and radio speaker sounds stay as they are.
link	main	3D sound (HRTF)	hrtf
link	main	Muffle through walls	muffle
link	main	Reverb	reverb
link	main	Advanced settings	advanced
key	main	Menu key	menu_key	novr	The key that opens this menu when the VR mod isn't loaded.
page	hrtf	main	Extended Sound Control: 3D sound
switch	hrtf	Binaural 3D sound	hrtf		Our own head model for sounds that have a place in the world (a stereo, a TV, a phone). Set the game's audio output to Stereo (Game Options > Audio): in Headphones or surround modes the game's own sound is used.
number	hrtf	Strength	hrtf_strength	0	1	0.05	100	%.0f%%		0 = the game's own sound, 100 = the full effect.
number	hrtf	Dull sounds behind you	hrtf_rear	0	1.5	0.05	100	%.0f%%		How much sounds behind you lose their treble, which is how the ear tells front from back.
switch	hrtf	Steam Audio HRTF	hrtf_sa		Use Steam Audio's measured HRTF (phonon.dll in the mod folder; a hrtf.sofa file there replaces its built-in set) instead of the built-in head model. Also gives height once the VR mod passes it.
switch	hrtf	Stereo sounds too (music, radio, TV)	hrtf_stereo		Also place stereo sounds in 3D from their left and right mix. Turn off if music sounds thin.
switch	hrtf	Close sounds keep direction	hrtf_close		A Sim or sound within 1 m stays where it is instead of sliding into the middle of your head (scenes, voices).
switch	hrtf	Mirror left and right	hrtf_mirror		Turn this on if a sound on your right plays on your left.
page	muffle	main	Extended Sound Control: muffle
switch	muffle	Muffle through walls	muffle		Sounds from another room, floor or behind a closed door get dull and quieter. Needs the VR mod to report the walls.
number	muffle	Strength	muffle_amount	0	1	0.05	100	%.0f%%		How strongly walls muffle.
number	muffle	Muffled treble	muffle_treble	200	4000	100	1	%.0f Hz		How dull a fully blocked sound gets. Lower = duller.
number	muffle	Level loss	muffle_loss	0	1	0.05	100	%.0f%%		How much quieter a fully blocked sound gets.
page	reverb	main	Extended Sound Control: reverb
switch	reverb	Room reverb	reverb		A tail on every sound with a place in the world, so rooms sound like rooms.
number	reverb	Amount	reverb_amount	0	1	0.05	100	%.0f%%		How much reverb is mixed in.
number	reverb	Room size	reverb_size	0	1	0.05	100	%.0f%%		Small packed room to big empty hall. Used until the VR mod reports the real room.
number	reverb	Soft furnishings	reverb_damp	0	1	0.05	100	%.0f%%		Carpets, curtains and furniture soak up the treble of the tail.
number	reverb	Outdoor reverb	reverb_outdoor	0	1	0.05	100	%.0f%%		How much reverb there is outside. Open air has very little.
page	advanced	main	Extended Sound Control: advanced
number	advanced	Muffle floor	clarity_floor	500	20000	500	1	%.0f Hz		With Clarity on: how muffled the furthest sounds may get. Higher = clearer.
number	advanced	UI sounds boost	ui_boost	1	4	0.05	100	%.0f%%		Menu clicks and notifications.
number	advanced	Menu music boost	menu_music_boost	1	4	0.05	100	%.0f%%		Main menu and loading music.
number	advanced	Ambient sounds are	ambient_bus	1	4	1	1	group %.0f		Which of the game's 4 hidden sound groups is ambience. Each change silences that group for 4 seconds so you can hear which one it is.
switch	advanced	Log the 3D panner	pan_log		Writes what the game's 3D sound panner is given to the log (groundwork for HRTF 3D sound; changes no sound).
switch	advanced	Look at the mixer after each lot load	probe_on_load		Writes the mixer to extendedsoundcontrol_probe.txt.
confirm	advanced	Look at the game's mixer now	probe		Writes the mixer and a count of playing sounds per group to extendedsoundcontrol_probe.txt (a few seconds).
)ESC"
