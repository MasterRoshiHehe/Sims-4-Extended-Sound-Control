// The settings menu, for the Sims 4 VR debug menu and this mod's own menu (esc_menu(), the format is
// jiggle/docs/MENU-EXPORT.md). One row per line, fields separated by TABs: keep the TABs when editing.
LR"ESC(page	main		Extended Sound Control
number	main	Master boost	boost	1	4	0.05	100	%.0f%%		Everything louder than the game allows, through its own limiter (no crackle).
number	main	Music boost	music_boost	1	4	0.05	100	%.0f%%		Radios, stereos and the score. On top of Master boost.
number	main	Sound effects boost	effects_boost	1	4	0.05	100	%.0f%%		On top of Master boost.
number	main	Voices boost	voices_boost	1	4	0.05	100	%.0f%%		Sims' voices. On top of Master boost.
number	main	Ambient boost	ambient_boost	1	4	0.05	100	%.0f%%		Birds, wind, rain, city hum (the game has no slider). Pick the group under Advanced settings first.
switch	main	Clarity	clarity		Stops the game muffling sounds below the Muffle floor and opens its treble cuts. Phone, TV and radio speaker sounds stay as they are.
link	main	Advanced settings	advanced
key	main	Menu key	menu_key	novr	The key that opens this menu when the VR mod isn't loaded.
page	advanced	main	Extended Sound Control: advanced
number	advanced	Muffle floor	clarity_floor	500	20000	500	1	%.0f Hz		With Clarity on: how muffled the furthest sounds may get. Higher = clearer.
number	advanced	UI sounds boost	ui_boost	1	4	0.05	100	%.0f%%		Menu clicks and notifications.
number	advanced	Menu music boost	menu_music_boost	1	4	0.05	100	%.0f%%		Main menu and loading music.
number	advanced	Ambient sounds are	ambient_bus	1	4	1	1	group %.0f		Which of the game's 4 hidden sound groups is ambience. Each change silences that group for 4 seconds so you can hear which one it is.
switch	advanced	Look at the mixer after each lot load	probe_on_load		Writes the mixer to extendedsoundcontrol_probe.txt.
confirm	advanced	Look at the game's mixer now	probe		Writes the mixer and a count of playing sounds per group to extendedsoundcontrol_probe.txt (a few seconds).
)ESC"
