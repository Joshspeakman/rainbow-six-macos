"""A game profile selects the matching executable AND its assets."""
from pathlib import Path


def select_profile(profiles, label, *, binary, exe, game, data):
    selected = dict(binary=str(binary), exe=str(exe), game=str(game), data=str(data), edition='gog')
    record = profiles.get(label)
    if isinstance(record, str):  # Existing installations store only the asset folder.
        selected['game'] = record
    elif isinstance(record, dict):
        if record.get('edition', 'gog') not in ('gog', 'broken-world'):
            raise ValueError('Unknown game edition in launcher profile.')
        for name in selected:
            if name in record:
                if not isinstance(record[name], str) or not record[name]:
                    raise ValueError(f'Invalid {name} in launcher profile.')
                selected[name] = record[name]
        if selected['edition'] == 'broken-world' and not all(record.get(k) for k in ('binary', 'exe', 'game')):
            raise ValueError('Broken World needs its own native executable and asset folder.')
    elif record is not None:
        raise ValueError('Invalid launcher profile.')
    return selected


def command(profile):
    return [profile['binary'], '--exe', profile['exe'], '--game', profile['game'], '--data', profile['data']]


def save_relative(edition='gog'):
    name = 'Dungeon Siege 2 Broken World' if edition == 'broken-world' else 'Dungeon Siege 2'
    return Path('drive_c/Users/player/Documents/My Games') / name / 'Save/SinglePlayer'
