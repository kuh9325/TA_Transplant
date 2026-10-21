#!/usr/bin/env python3
"""Summarize retained native mission evidence without promoting smoke to wins.

Modified 2026-10-20 for publication: public-safe provenance defaults.
Generated manifests remain local audit output and require their own review.
"""
import argparse
from collections import Counter
import csv
import hashlib
import json
from pathlib import Path
import re


def ci(mapping, key, default=None):
    return next((v for k, v in mapping.items() if k.lower() == key.lower()), default)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--data-provenance', default='Owner-supplied external retail data',
                        help='Public-safe data description; omit personal filesystem paths')
    args = parser.parse_args()
    root = args.evidence
    records = []
    for path in (root / 'sweep-final').glob('batch-*/*.json'):
        obj = json.loads(path.read_text())
        if isinstance(obj, dict) and 'runs' in obj:
            records.append(obj)
    records.sort(key=lambda m: m['ordinal'])
    assert len(records) == 175 and len({m['id'].upper() for m in records}) == 175
    capabilities = json.loads((root / 'catalog/unit-capabilities.json').read_text())
    outcome_directory = root / 'outcomes-final'
    outcomes = {d['id'].upper(): d for p in outcome_directory.glob('*.json')
                if (d := json.loads(p.read_text()))}
    assert len(outcomes) == 175, 'A complete final-code outcome run is required'
    completions = {}
    for path in root.glob('completion-*/*.json'):
        d = json.loads(path.read_text())
        if 'outcome' in d:
            d['evidence_file'] = str(path.relative_to(root))
            d['commands_admitted'] = bool(d['commands'])
            d['save_continuation_tested'] = (path.parent / (d['difficulty'] + '-mid.rwesave')).is_file()
            d['save_continuation_equal'] = d['save_continuation_tested'] and d['save_continuation_equal']
            completions.setdefault(d['id'].upper(), []).append(d)
    extended = {}
    for pattern in ['cc03-long/*.json', 'exp1ac08-long/*.json', 'overlap-long-*/*.json']:
        for path in root.glob(pattern):
            d = json.loads(path.read_text())
            if isinstance(d, dict) and 'runs' in d:
                extended.setdefault(d['id'].upper(), []).append(dict(
                    evidence_file=str(path.relative_to(root)), runs=d['runs']))
    result = []
    for m in records:
        header = ci(m['raw_ota'], 'GlobalHeader')
        schemas = []
        for key, s in header.items():
            if not isinstance(s, dict) or not key.lower().startswith('schema'):
                continue
            units = ci(s, 'units', {})
            ordered = sorted(units.items(), key=lambda p: int(re.sub(r'\D', '', p[0])))
            placements = [dict(ordinal=int(re.sub(r'\D', '', k)), **u) for k, u in ordered]
            scripts = [dict(unit_ordinal=u['ordinal'], unit_type=ci(u, 'Unitname'),
                            player=ci(u, 'Player'), commands=ci(u, 'InitialMission'))
                       for u in placements if ci(u, 'InitialMission', '').strip()]
            counts = Counter(ci(u, 'Unitname', '').upper() for u in placements)
            authored_xp = [dict(unit_ordinal=u['ordinal'], kills=int(ci(u, 'Kills', 0)))
                           for u in placements if int(ci(u, 'Kills', 0))]
            present = set(counts)
            referenced = set(re.findall(r'\b(?:ARM|COR)[A-Z0-9_]+\b',
                                        ' '.join(s['commands'] for s in scripts).upper()))
            mechanics = {}
            for mechanic in ['emp', 'veterancy', 'kamikaze', 'mine', 'bomber', 'aircraft_servicing',
                             'nuclear_missile', 'transport', 'reclaim', 'capture', 'cloak']:
                mechanics[mechanic] = dict(
                    placed_types=sorted(t for t in present if capabilities.get(t, {}).get(mechanic)),
                    explicitly_scripted_types=sorted(t for t in referenced if capabilities.get(t, {}).get(mechanic)))
            schemas.append(dict(schema=key, difficulty=ci(s, 'Type'), ai_profile=ci(s, 'aiprofile'),
                starting_units=placements, starting_unit_counts=dict(counts),
                starting_resources_authored={k: ci(s, k) for k in ['HumanMetal', 'HumanEnergy', 'ComputerMetal', 'ComputerEnergy']},
                initial_experience=dict(live_kills=0, authored_kills_metadata=authored_xp,
                                        note='OTA Kills is retained metadata, not starting live experience'),
                scripted_events=scripts, timers=dict(
                    creation_countdowns=[dict(unit_ordinal=u['ordinal'], ticks=ci(u, 'CreationCountdown'))
                                         for u in placements if ci(u, 'CreationCountdown') is not None],
                    wait_commands=[s for s in scripts if re.search(r'(?:^|,)\s*w(?:a|\s)', s['commands'], re.I)]),
                environmental_configuration={k: v for k, v in s.items() if not isinstance(v, dict)},
                special_mechanics=mechanics,
                missing_unit_definitions=sorted(present - set(capabilities))))
        probe = outcomes.get(m['id'].upper())
        objective_ok = bool(probe and probe['probes'] and all(p['passed'] for p in probe['probes']))
        runs = m['runs']
        assert len(runs) == 3
        complete_attempts = completions.get(m['id'].upper(), [])
        wins = [a for a in complete_attempts if a['outcome'] == 'VICTORY' and not a['fixture_edits']
                and a['ai_enabled'] and a['commands'] and a['save_continuation_equal']]
        resource_errors = sorted({error for r in runs for error in r['spawn_failures']})
        data_ok = not resource_errors and all(r['unresolved_features'] == 0 for r in runs)
        sim_ok = all(r['ticks_completed'] == 300 and not r['invalid_state'] and not r.get('first_orphan_aim') for r in runs)
        save_ok = all(r.get('save_initial_hash_equal') and r.get('save_objectives_equal')
                      and not r.get('save_first_divergence_tick') and not r.get('save_exception') for r in runs)
        levels = {'0': data_ok, '1': sim_ok, '2': objective_ok, '3': objective_ok, '4': bool(wins)}
        objectives = runs[0]['objective_transitions'][0]['state']
        entry = {k: m[k] for k in ['ordinal', 'id', 'name', 'campaign', 'descriptor', 'mission_index', 'campaign_size',
                                      'map', 'player_faction', 'opposing_faction']}
        entry.update(content_family='Battle Tactics' if m['campaign'].startswith('Battle Tactics') else
                     'Core Contingency' if 'Core Contingency' in m['campaign'] or m['campaign'] == 'Krogoth Encounter' else 'Original',
                     header_metadata={k: v for k, v in header.items() if not isinstance(v, dict)},
                     schemas=schemas, victory_conditions=objectives['victory'], defeat_conditions=objectives['defeat'],
                     relevant_objective_timers=[o for o in objectives['victory'] + objectives['defeat'] if 'Timer' in o['key']],
                     validation=dict(independent_levels=levels, highest_proven_level=max(int(k) for k, v in levels.items() if v),
                                     tested_difficulties=['Easy', 'Medium', 'Hard'],
                                     outcome_fixture_difficulty='Medium', outcome_fixture_scope='actual header, synthetic prerequisites, AI disabled',
                                     full_completion_difficulties=sorted(a['difficulty'] for a in wins), save_continuation_300_ticks=save_ok),
                     metadata_issues=dict(resource_errors=resource_errors,
                                          script_resolution_errors={r['difficulty']: r['script_errors'] for r in runs},
                                          unsupported_script_segments={r['difficulty']: r['unsupported_commands'] for r in runs}),
                     automated_coverage=dict(objective_probe=probe, actual_completion_attempts=complete_attempts,
                        shared_tests=['MissionObjectives.test.cpp: defaults, death, capture, position, timers, combination',
                                      'MissionInitialCommand.test.cpp and UnitBehaviorService.test.cpp: script mechanisms',
                                      'CampaignProgress.test.cpp / EndMissionScene_util.test.cpp / SavedGame.test.cpp',
                                      'Emp/Kamikaze/Bomber/AircraftService/DestructibleFeatures/Veterancy.test.cpp']),
                     bounded_execution=runs, extended_execution=extended.get(m['id'].upper(), []))
        result.append(entry)
    counts = {f'level_{i}': sum(m['validation']['independent_levels'][str(i)] for m in result) for i in range(5)}
    manifest = dict(schema_version=1, audit_date='2026-10-18', requested_report_date='2026-10-17',
        provenance=dict(data_path=args.data_provenance,
                        corpus='Current local mounted data, including installed overrides; not pristine retail certification',
                        evidence_directory=root.name, entry_sha256=hashlib.sha256((root / 'entry.json').read_bytes()).hexdigest()),
        policy=dict(levels_are_independent=True, level_0_requires_no_unresolved_placed_units_or_features=True,
                    level_2_means_focused_mechanisms_not_exhaustive_script_branches=True,
                    level_3_allows_explicit_synthetic_prerequisites=True,
                    nuclear_missile_exposure_marker='Broad stockpile-and-cruise proxy; includes ARMEMP and CORTRON as well as ARMSILO/CORSILO',
                    level_4_requires_real_commands_unmodified_start_and_live_ai=True),
        counts=dict(inventoried=175, difficulty_runs=525, extended_missions=len(extended), **counts),
        disposition=dict(automated_qa='PARTIAL', retail_parity='PARTIAL', human_qa='PENDING',
                         owner_review='STOP', note='Full completion remains unproven outside the listed command-driven wins'),
        performance=json.loads((root / 'performance/summary.json').read_text()), missions=result)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'MISSION_COMPLETION_MANIFEST_20261017.json').write_text(json.dumps(manifest, ensure_ascii=False, separators=(',', ':')) + '\n')
    fields = ['id', 'name', 'campaign', 'content_family', 'map', 'player_faction', 'opposing_faction',
              'level_0', 'level_1', 'level_2', 'level_3', 'level_4', 'highest_proven_level',
              'full_completion_difficulties', 'resource_errors', 'script_error_count', 'unsupported_segment_count',
              'save_continuation', 'victory_conditions', 'defeat_conditions']
    with (args.output / 'MISSION_COMPLETION_SUMMARY_20261017.csv').open('w', newline='') as file:
        writer = csv.DictWriter(file, fieldnames=fields); writer.writeheader()
        for m in result:
            row = {k: m[k] for k in fields if k in m}
            row.update({f'level_{i}': m['validation']['independent_levels'][str(i)] for i in range(5)})
            row.update(highest_proven_level=m['validation']['highest_proven_level'],
                       full_completion_difficulties=';'.join(m['validation']['full_completion_difficulties']),
                       resource_errors=';'.join(m['metadata_issues']['resource_errors']),
                       script_error_count=sum(len(r['script_errors']) for r in m['bounded_execution']),
                       unsupported_segment_count=sum(len(r['unsupported_commands']) for r in m['bounded_execution']),
                       save_continuation=m['validation']['save_continuation_300_ticks'])
            for side in ['victory_conditions', 'defeat_conditions']:
                row[side] = ';'.join(o['key'] + '(' + ','.join(o['params']) + ')' for o in m[side])
            writer.writerow(row)
    print(json.dumps(manifest['counts'], indent=2))


if __name__ == '__main__':
    main()
