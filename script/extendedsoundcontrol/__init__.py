"""Extended Sound Control (formerly Claude Sound): loads extendedsoundcontrol.dll (a Python extension
module, the game's Python has no ctypes). Volume beyond the game's 100 % (master and per category, through
the game's own limiter), Clarity (less muffling), and a mixer probe (extendedsoundcontrol_probe.txt).

Its own menu (menu.py) opens with the menu hotkey ("." by default) when Sims4VR isn't installed.
Console (Ctrl+Shift+C): esc.menu | esc.boost <percent> | esc.clarity <0/1> [floor Hz] | esc.probe | esc.status
"""
import os

VERSION = '0.6.0'
DLL_NAME = 'extendedsoundcontrol.dll'

_mod = None
_probed_lots = 0
_hotkey_alarm = None


class _AlarmOwner:
    pass


_alarm_owner = _AlarmOwner()


def _find_dll():
    path = os.path.dirname(os.path.abspath(__file__))
    for _ in range(5):
        candidate = os.path.join(path, DLL_NAME)
        if os.path.isfile(candidate):
            return candidate
        parent = os.path.dirname(path)
        if parent == path:
            break
        path = parent
    return None


def _load():
    global _mod
    path = _find_dll()
    if path is None:
        return
    import importlib.machinery
    import importlib.util
    loader = importlib.machinery.ExtensionFileLoader('_extendedsoundcontrol', path)
    spec = importlib.util.spec_from_file_location('_extendedsoundcontrol', path, loader=loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    _mod = mod


def _log(text):
    if _mod is not None:
        try:
            _mod.log(text)
        except Exception:
            pass


def _notify(text, connection=None):
    """Show a command's answer as a game notification too (seen in VR, where the console isn't),
    built the way the game's own ui.dialog.notification_test does."""
    try:
        import services
        from sims4.localization import LocalizationHelperTuning
        from ui.ui_dialog_notification import UiDialogNotification
        client = services.client_manager().get(connection) if connection is not None else None
        if client is None:
            client = services.client_manager().get_first_client()
        sim = client.active_sim if client is not None else None
        dialog = UiDialogNotification.TunableFactory().default(
            sim, text=lambda **_: LocalizationHelperTuning.get_raw_text(text),
            title=lambda **_: LocalizationHelperTuning.get_raw_text('Extended Sound Control'))
        dialog.show_dialog()
    except Exception as e:
        _log('Notification failed: {!r}'.format(e))


def _install():
    import zone
    original = zone.Zone.on_loading_screen_animation_finished

    def on_loading_screen_animation_finished(self, *args, **kwargs):
        result = original(self, *args, **kwargs)
        global _probed_lots
        try:
            if _mod is not None:
                _mod.rescan()
            if _mod is not None and (_mod.get('probe_on_load') or 0) != 0:
                _probed_lots += 1
                _mod.probe('lot load {}'.format(_probed_lots))
        except Exception as e:
            _log('Lot load probe failed: {!r}'.format(e))
        _start_hotkey()
        return result

    zone.Zone.on_loading_screen_animation_finished = on_loading_screen_animation_finished


def _open_menu():
    try:
        from extendedsoundcontrol import menu
        menu.open_menu(_mod, _log)
    except Exception as e:
        _log('Menu failed: {!r}'.format(e))


def _start_hotkey():
    """Asks the DLL for menu hotkey presses four times a second (real time, so it works while paused)."""
    global _hotkey_alarm
    if _mod is None:
        return
    import alarms
    import clock
    if _hotkey_alarm is not None:
        try:
            alarms.cancel_alarm(_hotkey_alarm)
        except Exception:
            pass

    def tick(_handle):
        try:
            if _mod.menu_presses() and not _mod.has_vr():
                _open_menu()
        except Exception as e:
            _log('Hotkey failed: {!r}'.format(e))

    _hotkey_alarm = alarms.add_alarm_real_time(_alarm_owner, clock.interval_in_real_seconds(0.25), tick, repeating=True)


def _status_text():
    if _mod is None:
        return 'extendedsoundcontrol.dll is not loaded.'
    known, running, probes, buses, boost = _mod.status()
    if not known:
        return 'Extended Sound Control {}: unknown game version, the mod stays off.'.format(VERSION)
    return 'Extended Sound Control {}: master boost {:.0f}% on {} buses. {}{} mixer look(s) in extendedsoundcontrol_probe.txt.'.format(
        VERSION, boost * 100, buses, 'Looking at the mixer now; ' if running else '', probes)


def _commands():
    import sims4.commands

    @sims4.commands.Command('esc.menu', command_type=sims4.commands.CommandType.Live)
    def esc_menu(_connection=None):
        _open_menu()

    @sims4.commands.Command('esc.status', command_type=sims4.commands.CommandType.Live)
    def sound_status(_connection=None):
        text = _status_text()
        sims4.commands.CheatOutput(_connection)(text)
        _notify(text, _connection)

    @sims4.commands.Command('esc.boost', command_type=sims4.commands.CommandType.Live)
    def sound_boost(percent: float = 100, _connection=None):
        if _mod is None:
            text = 'extendedsoundcontrol.dll is not loaded.'
        else:
            _mod.set('boost', max(100.0, min(float(percent), 400.0)) / 100.0)
            text = _status_text()
        sims4.commands.CheatOutput(_connection)(text)
        _notify(text, _connection)

    @sims4.commands.Command('esc.clarity', command_type=sims4.commands.CommandType.Live)
    def sound_clarity(on: int = 1, floor: float = 0, _connection=None):
        if _mod is None:
            text = 'extendedsoundcontrol.dll is not loaded.'
        else:
            _mod.set('clarity', 1 if on else 0)
            if floor > 0:
                _mod.set('clarity_floor', floor)
            text = 'Clarity {}: low-passes kept at {:.0f} Hz or above.'.format(
                'on' if _mod.get('clarity') else 'off', _mod.get('clarity_floor') or 0)
        sims4.commands.CheatOutput(_connection)(text)
        _notify(text, _connection)

    @sims4.commands.Command('esc.probe', command_type=sims4.commands.CommandType.Live)
    def sound_probe(_connection=None):
        if _mod is None:
            text = 'extendedsoundcontrol.dll is not loaded.'
        else:
            _mod.probe('console')
            text = 'Looking at the game\'s mixer (a few seconds): extendedsoundcontrol_probe.txt in the mod folder.'
        sims4.commands.CheatOutput(_connection)(text)
        _notify(text, _connection)


try:
    _load()
    _install()
    _commands()
    _log('Script {} loaded'.format(VERSION))
except Exception as e:
    _log('Install failed: {!r}'.format(e))
