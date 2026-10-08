"""Test commands use retail Skrit APIs. No lifted game code or assets here."""
import json
import os
from pathlib import Path
import re
import time


def identifier(value):
    if not re.fullmatch(r'[a-zA-Z_][a-zA-Z_0-9]*', value):
        raise ValueError('Invalid content identifier')
    return value


def guarded(body):
    # In particular, never submit test mutations in Internet/LAN multiplayer.
    return '''if (WorldState.CurrentState == WS_SP_INGAME) {
        Go h$ = Server.GetScreenHero();
        if (h$ != NULL) { %s }
        else { NativeLabResult("ERROR: No screen hero."); }
    } else { NativeLabResult("ERROR: Load a single-player world and close menus first."); }''' % body


def command(action, entry=None, **options):
    entry = entry or {}
    name = identifier(entry['id']) if 'id' in entry else ''
    if action == 'status':
        return guarded('''string m$; string state$="vulnerable";
            if(h$.Aspect.IsInvincible) {state$="invincible";}
            StringTool.Assignf(m$,"Ready | %s | %s",state$,
                WorldMap.GetRegionNameForNode(h$.Placement.Position.Node));
            NativeLabResult(m$);''')
    if action == 'invincible':
        value = 'true' if options.get('enabled', True) else 'false'
        body = f'h$.Aspect.SetIsInvincible({value}); NativeLabResult("Invincibility {value}");'
    elif action == 'prepare':
        body = '\n'.join(f'h$.Actor.RSSetNaturalSkillLevel("{skill}",100);'
                         for skill in ('uber', 'Melee', 'Ranged', 'Combat Magic', 'Nature Magic'))
        for skill in options.get('skills', []):
            skill_name = identifier(skill['id'])
            level = max(1, min(100, int(skill['level'])))
            body += f'\nh$.Actor.RSSetNaturalSkillLevel("{skill_name}",{level});'
        body += '\n' + '\n'.join(f'h$.Actor.SetSkillLevel("{s}",500);' for s in ('strength', 'dexterity', 'intelligence'))
        body += '''h$.Aspect.SetIsInvincible(true);
            h$.Aspect.RSSetCurrentLife(h$.Aspect.MaxLife);
            h$.Aspect.SSetMaxMana(100000); h$.Aspect.RSSetCurrentMana(100000);
            NativeLabResult("Prepared hero: invincible, level 100 skills, 500 attributes, full life and mana.");'''
    elif action == 'refill':
        body = '''h$.Aspect.RSSetCurrentLife(h$.Aspect.MaxLife);
            h$.Aspect.RSSetCurrentMana(h$.Aspect.MaxMana); h$.Actor.SetActiveSkillReady(true);
            NativeLabResult("Life, mana and power refilled.");'''
    elif action == 'wound':
        body = '''h$.Aspect.RSSetCurrentLife(h$.Aspect.MaxLife * 0.5);
            NativeLabResult("Hero set to half life for healing tests.");'''
    elif action == 'travel':
        map_name = identifier(entry['map'])
        body = f'''if (StringTool.SameNoCase(WorldMap.GetMapName(),"{map_name}")) {{
            my SiegePos p$; my CameraPosition c$;
            if (WorldMap.GetFirstStartingPosition("{name}",p$,c$)) {{
                Game.STeleportPlayerParty(h$.Goid,8.0,p$,c$);
                NativeLabResult("Travel requested. Wait for streaming/cinematics before the next test.");
            }} else {{ NativeLabResult("ERROR: Destination unavailable in this map."); }}
        }} else {{ NativeLabResult("ERROR: Load the matching map first."); }}'''
    elif action in ('spawn', 'give', 'equip', 'cast'):
        # A bounded list permits explicit cleanup and avoids untracked flood spawns.
        count = max(1, min(16, int(options.get('count', 1)))) if action == 'spawn' else 1
        start = '''my SiegePos p$; bool spot$ = AIQuery.GetPathablePositionAroundSource(
            h$,h$.Mind.PersonalSpaceRange,5.0,3,p$);''' if action == 'spawn' else 'my SiegePos p$; p$=h$.Placement.Position; bool spot$=true;'
        power = entry.get('power', False) and action == 'cast'
        create = f'Goid g$ = h$.Actor.SCreatePowerObject("{name}");' if power else f'''
            my GoCloneReq r$; r$.Init("{name}"); r$.StartingPos=p$;
            r$.SnapToTerrain={'true' if action == 'spawn' else 'false'};
            Goid g$=GoDb.SCloneGo(r$);'''
        use = ''
        if action == 'spawn':
            if options.get('immortal', True):
                use += 'if(g$.Go.HasAspect) {g$.Go.Aspect.SetIsInvincible(true);}'
            if options.get('friendly'):
                use += 'if(g$.Go.HasActor) {g$.Go.Actor.SSetAlignment(AA_GOOD);}'
            use += 'NativeLabResult("Spawned; last object is the test target.");'
        else:
            if not power:
                use += '''h$.Inventory.RSAdd(g$.Go,IL_MAIN,AO_COMMAND,true);'''
            if action == 'give':
                use += 'NativeLabResult("Item created; check the hero inventory.");'
            elif action == 'equip':
                slot = identifier(entry.get('slot', '').upper())
                if not slot.startswith('ES_'):
                    raise ValueError('This item has no equipment slot; use Give item instead.')
                use += f'''h$.Inventory.RSEquip({slot},g$,AO_COMMAND);
                    NativeLabResult("Equip requested; check appearance and inventory.");'''
            else:
                use += 'if(g$.Go.HasMagic) {' + cast_body(power, options.get('target', 'auto'), options.get('direct', True)) + '} else {NativeLabResult("ERROR: Template has no magic component.");}'
        body = f'''if(NativeLabCount()+{count} <= 512) {{
            int i$=0; while(i$<{count}) {{ {start}
                if(spot$) {{ {create}
                    if(g$.IsValid()) {{ NativeLabTrack(g$,{'true' if action == 'spawn' else 'false'}); {use} }}
                    else {{ NativeLabResult("ERROR: Template could not be created."); }}
                }} else {{ NativeLabResult("ERROR: No nearby walkable spawn point."); }}
                i$+=1;
            }}
        }} else {{ NativeLabResult("ERROR: Test object limit reached. Clean up before spawning more."); }}'''
    elif action == 'power':
        # Actual active-skill route: preserves stance checks, animations and
        # delivery semantics (melee/ranged powers may enhance the next attack).
        target = options.get('target', 'auto')
        use_self = target == 'self' or (target == 'auto' and 'tt_self' in entry.get('targets', '').lower())
        body = f'''h$.Actor.RSSetCurrentActiveSkill("{name}");
            h$.Actor.SetActiveSkillReady(true);
            Goid t$={'h$.Goid' if use_self else 'NativeLabTarget()'};
            if(t$.IsValid()) {{
                if(Skills.CanUseSkillOnObject("{name}",h$,t$.Go)) {{
                    h$.Mind.RSDoJob(MakeJobReq(JAT_CAST_POWER,JQ_ACTION,QP_CLEAR,AO_USER,t$));
                    NativeLabResult("Power job queued; some powers enhance the next weapon attack.");
                }} else {{NativeLabResult("ERROR: Power selected, but target, skill or weapon stance does not meet its requirements.");}}
            }} else {{NativeLabResult("ERROR: Power selected and recharged. Spawn a target first.");}}'''
    elif action == 'corpse':
        body = '''Goid t$=NativeLabTarget(); if(t$.IsValid() && t$.Go.HasActor) {
            t$.Go.Aspect.SetIsInvincible(false); t$.Go.Aspect.RSSetCurrentLife(0); t$.Go.Aspect.SSetLifeState(LS_DEAD_NORMAL);
            if(IsDead(t$.Go.Aspect.LifeState)) {NativeLabResult("Target killed for corpse/resurrection tests.");}
            else {NativeLabResult("ERROR: The target could not be killed; its scene scripts may protect it.");}
        } else { NativeLabResult("ERROR: Spawn a target first."); }'''
    elif action == 'party_target':
        member = max(0, min(5, int(options.get('member', 1)) - 1))
        # Existing party members are targets, never cleanup-owned lab spawns.
        body = f'''GopColl members$=Server.GetScreenParty().GetChildren();
            if(members$.Size()>{member}) {{
                Go target$=members$.Get({member}); NativeLabSetTarget(target$.Goid);
                NativeLabResult(target$.TemplateName);
            }} else {{NativeLabResult("ERROR: That party member is not present.");}}'''
    elif action == 'cleanup':
        body = '''int i$=0; while(i$<NativeLabCount()) {
            Goid g$=NativeLabObject(i$);
            if(g$.IsValid()) { GoDb.SMarkGoAndChildrenForDeletion(g$); } i$+=1;
        } NativeLabClear(); NativeLabResult("Tracked test objects removed.");'''
    else:
        raise ValueError('Unknown test action')
    return guarded(body)


def cast_body(power, target, direct=True):
    if target not in ('auto', 'self', 'target', 'ground'):
        raise ValueError('Unknown spell target')
    select = {'self': 'Goid t$=h$.Goid;', 'target': 'Goid t$=NativeLabTarget();',
              'ground': 'Goid t$=NativeLabTarget();',
              'auto': '''Goid t$=h$.Goid;
                  if(g$.Go.Magic.IsOffensive() || g$.Go.Magic.HasTargetTypeFlags(TT_DEAD_ACTOR)) {
                      t$=NativeLabTarget(); }'''}[target]
    invoke = '''g$.Go.Magic.SCast(t$.Go); NativeLabResult("Magic invoked through the effect path (no casting animation). Visually verify the result.");''' if power or direct else '''
        if(h$.Mind.RSCast(t$,g$,QP_CLEAR,AO_COMMAND)) { NativeLabResult("Cast queued; visually verify its result."); }
        else { NativeLabResult("ERROR: Engine rejected the cast job."); }'''
    ground = '''my SiegePos pos$; pos$=h$.Placement.Position;
            if(t$.IsValid()) {pos$=t$.Go.Placement.Position;}
            if(h$.Mind.RSCastAtPosition(pos$,g$,QP_CLEAR,AO_COMMAND)) {
                NativeLabResult("Ground cast queued; visually verify its result.");
            } else {NativeLabResult("ERROR: Engine rejected the ground cast.");}'''
    if direct and not power:
        ground = '''if(g$.Go.Magic.IsCastableOnGround()) {
            my GoCloneReq pointReq$; pointReq$.Init("point");
            pointReq$.StartingPos=h$.Placement.Position;
            if(t$.IsValid()) {pointReq$.StartingPos=t$.Go.Placement.Position;}
            pointReq$.SnapToTerrain=true; pointReq$.ForceClientAllowed=true;
            Goid point$=GoDb.SCloneGo(pointReq$);
            if(point$.IsValid()) {
                g$.Go.Magic.SCast(point$.Go); GoDb.StartExpiration(point$.Go,true);
                NativeLabResult("Ground magic invoked through the effect path (no casting animation). Visually verify the result.");
            } else {NativeLabResult("ERROR: Could not create the ground target.");}
        } else {NativeLabResult("ERROR: This spell does not support ground targets.");}'''
    if target == 'ground' and not power:
        return 'h$.Aspect.RSSetCurrentMana(h$.Aspect.MaxMana);' + select + ground
    regular = '''
        if(t$.IsValid()) {
            if(g$.Go.Magic.IsCastableOn(t$.Go)) { %s }
            else {NativeLabResult("ERROR: Spell requires a different target, state or equipment. Try Self, Target or Ground.");}
        } else { NativeLabResult("ERROR: Spawn a target first."); }''' % invoke
    if target == 'auto' and not power:
        regular = '''if(g$.Go.Magic.IsCastableOnGround() &&
            (!t$.IsValid() || !g$.Go.Magic.IsCastableOn(t$.Go))) { %s } else { %s }''' % (ground, regular)
    return 'h$.Aspect.RSSetCurrentMana(h$.Aspect.MaxMana);' + select + regular


class Bridge:
    """One outstanding command; atomic mailbox and durable, honest history."""
    def __init__(self, data):
        self.directory = Path(data) / 'lab'
        self.directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.pending = None
        self.sequence = time.time_ns() % 2000000000

    def send(self, action, entry=None, **options):
        if self.pending:
            raise RuntimeError('Wait for the current test command to finish.')
        code = command(action, entry, **options)
        if len(code.encode()) >= 16000:
            raise ValueError('Test command is too long')
        self.sequence += 1
        record = {'id': self.sequence, 'time': time.time(), 'action': action,
                  'content': (entry or {}).get('id', ''), 'options': options,
                  'status': 'submitted', 'backend': 'linux-fixed-function'}
        if action != 'status': self.record(record)
        path = self.directory / 'request.tmp'
        path.write_text(f'{self.sequence} exec\n{code}', encoding='ascii')
        os.chmod(path, 0o600)
        path.replace(self.directory / 'request')
        self.pending = record

    def record(self, record):
        with (self.directory / 'history.jsonl').open('a') as f:
            f.write(json.dumps(record) + '\n')

    def poll(self):
        if not self.pending: return None
        try:
            parts = (self.directory / 'response').read_text().strip().split('\t', 2)
            if len(parts) != 3 or parts[0] != str(self.pending['id']): return self.timeout()
        except FileNotFoundError:
            return self.timeout()
        record = dict(self.pending, status=parts[1], message=parts[2], finished=time.time())
        if record['message'].startswith('ERROR:'): record['status'] = 'error'
        self.pending = None
        if record['action'] != 'status': self.record(record)
        return record

    def timeout(self):
        if time.time() - self.pending['time'] < 120: return None
        # Never automatically retry a mutation: it might have reached the game.
        record = dict(self.pending, status='unconfirmed', message='No reply after 120 seconds. Check the game and log; this command was not retried.')
        self.pending = None
        (self.directory / 'request').unlink(missing_ok=True)
        if record['action'] != 'status': self.record(record)
        return record
