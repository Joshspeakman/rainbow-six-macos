"""Read test-lab catalogs from the player's own Dungeon Siege II archives."""
import mmap
from pathlib import Path
import re
import struct
import zlib


class Tank:
    def __init__(self, path):
        self.path = Path(path)
        self.file = self.path.open('rb')
        self.data = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            if self.read(0, 8) not in (b'DSigTank', b'DSg2Tank'):
                raise ValueError(f'Not a game archive: {self.path.name}')
            dirs_at, files_at, self.payload = self.u32(12), self.u32(16), self.u32(24)
            dirs = {}
            for i in range(self.u32(dirs_at)):
                offset = self.u32(dirs_at + 4 + 4 * i)
                at = dirs_at + offset
                dirs[offset] = (self.u32(at), self.read(at + 18, self.u16(at + 16)).decode('latin1'))

            def directory(offset):
                parts = []
                for _ in range(64):
                    parent, name = dirs.get(offset, (offset, ''))
                    if not name:
                        return '/'.join(reversed(parts))
                    parts.append(name)
                    if parent == offset:
                        return '/'.join(reversed(parts))
                    offset = parent
                raise ValueError('Archive directory loop')

            self.entries = {}
            for i in range(self.u32(files_at)):
                at = files_at + self.u32(files_at + 4 + 4 * i)
                name = directory(self.u32(at)) + '/' + self.read(at + 30, self.u16(at + 28)).decode('latin1')
                self.entries[name.lower().lstrip('/')] = at
        except Exception:
            self.close()
            raise

    def read(self, at, count):
        if at < 0 or count < 0 or at + count > len(self.data):
            raise ValueError('Truncated game archive')
        return self.data[at:at + count]

    def u32(self, at):
        return struct.unpack('<I', self.read(at, 4))[0]

    def u16(self, at):
        return struct.unpack('<H', self.read(at, 2))[0]

    def get(self, name):
        at = self.entries[name.lower()]
        size, offset, fmt = self.u32(at + 4), self.u32(at + 8), self.u16(at + 24)
        if size > 64 * 1024 * 1024:
            raise ValueError('Oversized catalog entry')
        if fmt == 0:
            return self.read(self.payload + offset, size)
        table = (at + 30 + self.u16(at + 28) + 4) & ~3
        chunk = self.u32(table + 4)
        if not chunk:
            raise ValueError('Zero archive chunk size')
        chunks = []
        for c in range((size + chunk - 1) // chunk):
            row = table + 8 + 16 * c
            usz, csz, extra, coff = (self.u32(row + j) for j in (0, 4, 8, 12))
            start = self.payload + offset + coff
            block = self.read(start, csz)
            raw = zlib.decompress(block) if csz < usz else block[:usz]
            if len(raw) + extra != usz:
                raise ValueError('Invalid archive chunk size')
            chunks.extend((raw, self.read(start + csz, extra)))
        return b''.join(chunks)[:size]

    def close(self):
        self.data.close()
        self.file.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


# GAS has nested blocks, typed assignments, multiline quoted values and C/C++
# comments. Keep strings intact: a regex over whole template blocks loses both
# nesting and inheritance (especially spell effects inherited from base spells).
TOKEN = re.compile(r'\s+|//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\[[^\]]*\]|[{};=]|[^\s{};=\[\]"]+', re.S)


def parse_gas(text, issues=None):
    issues = issues if issues is not None else []
    tokens = [m.group() for m in TOKEN.finditer(text)
              if not m.group().isspace() and not m.group().startswith(('//', '/*'))]
    root = {'name': '', 'type': '', 'attrs': {}, 'children': []}
    stack = [root]
    i = 0
    while i < len(tokens):
        token = tokens[i]
        if token.startswith('[') and i + 1 < len(tokens) and tokens[i + 1] == '{':
            header = token[1:-1].strip()
            fields = dict(re.findall(r'(\w+)\s*:\s*([^,]+)', header))
            node = {'name': fields.get('n', header).strip().lower(),
                    'type': fields.get('t', '').strip().lower(), 'attrs': {}, 'children': []}
            if node['type'] == 'template' and len(stack) > 1:
                issues.append('Recovered an unclosed template block')
                stack = [root]
            stack[-1]['children'].append(node)
            stack.append(node)
            i += 2
        elif token == '}':
            if len(stack) == 1:
                # A few shipped files contain stray braces; the engine accepts
                # them too. Keep subsequent templates instead of losing a file.
                issues.append('Ignored a stray closing brace')
            else:
                stack.pop()
            i += 1
        elif token in ('{', ';'):
            i += 1
        else:
            key = []
            while i < len(tokens) and tokens[i] not in ('=', ';', '{', '}') and not tokens[i].startswith('['):
                key.append(tokens[i]); i += 1
            if i < len(tokens) and tokens[i] == '=':
                i += 1; value = []
                while i < len(tokens) and tokens[i] not in (';', '}'):
                    value.append(tokens[i]); i += 1
                raw = ' '.join(value)
                if raw.startswith('"') and raw.endswith('"'):
                    raw = raw[1:-1].replace('\\"', '"').replace('\\n', '\n')
                if key:
                    stack[-1]['attrs'][key[-1].lower()] = raw
            elif not key:
                i += 1
    if len(stack) != 1:
        issues.append('Recovered an unclosed GAS block at end of file')
    return root['children']


def flatten(node, prefix=''):
    result = {prefix + key: value for key, value in node['attrs'].items()}
    for child in node['children']:
        result.update(flatten(child, prefix + child['name'] + '.'))
    return result


def build_catalog(game):
    game = Path(game)
    archives = sorted((game / 'Resources').glob('*.ds2res')) + sorted((game / 'Maps').glob('*.ds2map'))
    templates, locations, effects, shaders, errors, powers, skills = {}, [], [], [], [], [], []
    for path in archives:
        with Tank(path) as tank:
            for name in tank.entries:
                if name.endswith('.fx'):
                    shaders.append({'id': name, 'label': Path(name).stem, 'source': path.name + ':' + name})
                is_template = name.startswith('world/contentdb/templates/') and name.endswith('.gas')
                is_location = name.endswith('/info/start_positions.gas')
                is_effect = name.startswith('world/global/effects/') and name.endswith('.gas')
                is_power = name == 'world/global/skills/active_skills.gas'
                is_skill = name == 'world/global/skills/passive_skills.gas'
                if not (is_template or is_location or is_effect or is_power or is_skill):
                    continue
                try:
                    nodes = parse_gas(tank.get(name).decode('latin1'))
                except (ValueError, zlib.error) as e:
                    errors.append(f'{path.name}:{name}: {e}')
                    continue
                if is_template:
                    for node in nodes:
                        if node['type'] == 'template':
                            templates[node['name']] = (flatten(node), path.name + ':' + name)
                elif is_location:
                    for parent in nodes:
                        for node in parent['children']:
                            if node['type'] != 'start_group':
                                continue
                            attrs = flatten(node)
                            locations.append({'id': node['name'], 'label': attrs.get('screen_name') or node['name'],
                                              'map': name.split('/')[2], 'description': attrs.get('description', ''),
                                              'position': attrs.get('start_position.position', ''),
                                              'source': path.name + ':' + name})
                elif is_skill:
                    for parent in nodes:
                        for node in parent['children']:
                            if 'max_level' in node['attrs']:
                                skills.append({'id': node['name'], 'level': int(node['attrs']['max_level'])})
                elif is_power:
                    for parent in nodes:
                        for node in parent['children']:
                            attrs = flatten(node)
                            powers.append({'id': node['name'], 'label': attrs.get('screen_name', node['name']),
                                           'description': attrs.get('summary', ''), 'targets': attrs.get('target', ''),
                                           'template': attrs.get('template', ''), 'category': attrs.get('type', ''),
                                           'level': attrs.get('max_level', '1'), 'stances': attrs.get('valid_stances', ''),
                                           'source': path.name + ':' + name,
                                           'effects': [v for k, v in attrs.items() if 'ffx' in k]})
                else:
                    for node in nodes:
                        effects.append({'id': node['name'], 'label': node['name'], 'source': path.name + ':' + name})
    resolved = {}

    def resolve(name, trail=()):
        if name in resolved:
            return resolved[name]
        if name not in templates or name in trail:
            return {}
        attrs, _ = templates[name]
        parent = attrs.get('specializes', '').lower()
        result = dict(resolve(parent, trail + (name,)))
        result.update(attrs)
        resolved[name] = result
        return result

    groups = {key: [] for key in ('gear', 'enemies', 'spells', 'objects')}
    for name, (_, source) in templates.items():
        attrs = resolve(name)
        # Base definitions are included separately by the UI's template filter;
        # only concrete entries participate in automatic sweeps.
        base = name.startswith(('base_', 'actor_', 'spell_base', 'go_')) or name.endswith(('_base', '_stats'))
        category = attrs.get('category_name', '').lower()
        if '/actors/evil/' in source:
            group = 'enemies'
        elif category == 'magic' or attrs.get('magic.is_power', '').lower() == 'true' or name.startswith(('spell_', 'base_spell_', 'power_')):
            group = 'spells'
        elif any(word in category for word in ('weapon', 'armor', 'ammunition', 'spellbook', 'potion', 'reagent', 'jewelry', 'ring', 'amulet')) or category == 'misc':
            group = 'gear'
        else:
            group = 'objects'
        refs = sorted({v for k, v in attrs.items() if ('ffx' in k or 'efct' in k or 'effect' in k) and v})
        groups[group].append({'id': name, 'label': attrs.get('common.screen_name', name),
                              'category': category, 'base': base, 'source': source,
                              'description': attrs.get('common.description', attrs.get('doc', '')),
                              'damage': attrs.get('magic.damage_type', ''),
                              'targets': attrs.get('magic.target_type_flags', ''),
                              'power': attrs.get('magic.is_power', 'false').lower() == 'true',
                              'slot': attrs.get('gui.equip_slot', ''),
                              'effects': refs})
    return {'version': 1, 'game': str(game), **groups, 'locations': locations,
            'effects': effects, 'shaders': shaders, 'powers': powers, 'skills': skills, 'errors': errors}
