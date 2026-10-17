"""Extended Sound Control's own menu (game picker dialogs), for players without Sims4VR (Sims4VR shows
these settings on its VR debug menu). Opens with the menu hotkey ("." by default, set in Advanced) or the
console command esc.menu. Basic settings on the first page, the rest under Advanced settings.

Built the way the game's own pickers are (ui/ui_dialog_picker.py): a single-select UiObjectPicker with
ObjectPickerRows; picking a setting opens a picker of its values, then the page comes back.
"""

BOOSTS = [1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9, 2.0, 2.25, 2.5, 2.75, 3.0, 3.5, 4.0]
FLOORS = [1000, 2000, 3000, 4000, 5000, 6000, 8000, 10000, 12000, 14000, 16000]
KEYS = [(0xBE, '. (full stop)'), (0xBC, ', (comma)'), (0x78, 'F9'), (0x79, 'F10'), (0x7A, 'F11'), (0x7B, 'F12'),
        (0x2D, 'Insert'), (0x24, 'Home'), (0, 'None')]


def _percent(v):
    return '{:.0f}%'.format(v * 100)


def _on_off(v):
    return 'On' if v else 'Off'


def _key_name(v):
    for code, name in KEYS:
        if code == int(v):
            return name
    return 'key {}'.format(int(v))


# (key, label, description, kind, values, shown)
BASIC = [
    ('boost', 'Master boost', 'Everything louder than the game allows, through its own limiter (no crackle).',
     'number', BOOSTS, _percent),
    ('music_boost', 'Music boost', 'Radios, stereos and the score. On top of Master boost.', 'number', BOOSTS, _percent),
    ('effects_boost', 'Sound effects boost', 'On top of Master boost.', 'number', BOOSTS, _percent),
    ('voices_boost', "Voices boost", "Sims' voices. On top of Master boost.", 'number', BOOSTS, _percent),
    ('ambient_boost', 'Ambient boost', 'Birds, wind, rain, city hum (the game has no slider). Pick the group under '
     'Advanced first.', 'number', BOOSTS, _percent),
    ('clarity', 'Clarity', "Stops the game muffling sounds below the Muffle floor and opens its treble cuts. "
     "Phone, TV and radio speaker sounds stay as they are.", 'switch', [1, 0], _on_off),
]
ADVANCED = [
    ('clarity_floor', 'Muffle floor', 'With Clarity on: how muffled the furthest sounds may get. Higher = clearer.',
     'number', FLOORS, lambda v: '{:.0f} Hz'.format(v)),
    ('ui_boost', 'UI sounds boost', 'Menu clicks and notifications.', 'number', BOOSTS, _percent),
    ('menu_music_boost', 'Menu music boost', 'Main menu and loading music.', 'number', BOOSTS, _percent),
    ('ambient_bus', 'Ambient sounds are', "Which of the game's 4 hidden sound groups is ambience. Each change "
     "silences that group for 4 seconds so you can hear which one it is.", 'number', [1, 2, 3, 4],
     lambda v: 'group {:.0f}'.format(v)),
    ('menu_key', 'Menu hotkey', 'The key that opens this menu (when Sims4VR is not installed).', 'number',
     [code for code, _ in KEYS], _key_name),
    ('probe_on_load', 'Look at the mixer after each lot load', 'Writes the mixer to extendedsoundcontrol_probe.txt.',
     'switch', [1, 0], _on_off),
    ('probe', "Look at the game's mixer now", 'Writes the mixer and a count of playing sounds per group to the '
     'mod folder (a few seconds).', 'action', None, None),
]
ADVANCED_ROW = 1000
BACK_ROW = 1001


def _raw(text):
    from sims4.localization import LocalizationHelperTuning
    return LocalizationHelperTuning.get_raw_text(text)


def _picker(title, text, rows, on_pick):
    """rows: [(option_id, name, description, selected)]; on_pick(option_id) when the player picks one."""
    import services
    from ui.ui_dialog_picker import UiObjectPicker, ObjectPickerRow
    client = services.client_manager().get_first_client()
    sim = client.active_sim if client is not None else None
    if sim is None:
        return False
    dialog = UiObjectPicker.TunableFactory().default(
        sim, text=lambda *_, **__: _raw(text), title=lambda *_, **__: _raw(title), min_selectable=1,
        picker_type=UiObjectPicker.UiObjectPickerObjectPickerType.OBJECT)
    for option_id, name, description, selected in rows:
        dialog.add_row(ObjectPickerRow(option_id=option_id, name=_raw(name), row_description=_raw(description),
                                       is_selected=selected))

    def listener(d):
        if d.accepted and d.picked_results:
            on_pick(d.picked_results[0])

    dialog.add_listener(listener)
    dialog.show_dialog()
    return True


def open_menu(mod, log, advanced=False):
    entries = ADVANCED if advanced else BASIC
    rows = []
    for i, (key, label, description, kind, values, shown) in enumerate(entries):
        if kind == 'action':
            rows.append((i, label, description, False))
        else:
            rows.append((i, '{}: {}'.format(label, shown(mod.get(key) or 0)), description, False))
    if advanced:
        rows.append((BACK_ROW, 'Back', 'The basic settings.', False))
    else:
        rows.append((ADVANCED_ROW, 'Advanced settings', 'Muffle floor, UI and menu music, which group is ambience, '
                     'the menu hotkey and diagnostics.', False))

    def picked(option_id):
        if option_id == ADVANCED_ROW:
            open_menu(mod, log, True)
        elif option_id == BACK_ROW:
            open_menu(mod, log, False)
        elif 0 <= option_id < len(entries):
            _open_value(mod, log, entries[option_id], advanced)

    title = 'Extended Sound Control' + (': advanced' if advanced else '')
    if not _picker(title, 'Pick a setting to change it.', rows, picked):
        log('Menu: no active Sim to show it to (open it while playing a lot)')


def _open_value(mod, log, entry, advanced):
    key, label, description, kind, values, shown = entry
    if kind == 'action':
        mod.set(key, 1)
        open_menu(mod, log, advanced)
        return
    current = mod.get(key) or 0
    rows = [(i, shown(v), '', abs(v - current) < 1e-3) for i, v in enumerate(values)]

    def picked(option_id):
        if 0 <= option_id < len(values):
            mod.set(key, float(values[option_id]))
        open_menu(mod, log, advanced)

    _picker(label, description, rows, picked)
