#!/usr/bin/env python3

import sys
import json
import os
import datetime

# Currently, this hook logs tools used in HANDSOFF_MODE to a centralized file.

hook_input = json.load(sys.stdin)

if os.getenv('HANDSOFF_MODE', '0').lower() not in ['1', 'true', 'on', 'enable']:
    sys.exit(0)

tool = hook_input['tool_name']
session = hook_input['session_id']
tool_input = hook_input.get('tool_input', {})

if os.getenv('HANDSOFF_DEBUG', '0').lower() in ['1', 'true', 'on', 'enable']:
    os.makedirs('.tmp', exist_ok=True)
    os.makedirs('.tmp/hooked-sessions', exist_ok=True)

    # Detect workflow state from session state file
    workflow = 'unknown'
    state_file = f'.tmp/hooked-sessions/{session}.json'
    if os.path.exists(state_file):
        try:
            with open(state_file, 'r') as f:
                state = json.load(f)
                workflow_type = state.get('workflow', '')
                if workflow_type == 'ultra-planner':
                    workflow = 'plan'
                elif workflow_type == 'issue-to-impl':
                    workflow = 'impl'
        except (json.JSONDecodeError, Exception):
            pass

    # Extract relevant object/target from tool_input
    target = ''
    if tool in ['Read', 'Write', 'Edit', 'NotebookEdit']:
        target = tool_input.get('file_path', '')
    elif tool == 'Bash':
        target = tool_input.get('command', '')
    elif tool == 'Grep':
        pattern = tool_input.get('pattern', '')
        path = tool_input.get('path', '')
        target = f'pattern={pattern}' + (f' path={path}' if path else '')
    elif tool == 'Glob':
        pattern = tool_input.get('pattern', '')
        path = tool_input.get('path', '')
        target = f'pattern={pattern}' + (f' path={path}' if path else '')
    elif tool == 'Task':
        subagent = tool_input.get('subagent_type', '')
        desc = tool_input.get('description', '')
        target = f'subagent={subagent} desc={desc}'
    elif tool == 'Skill':
        skill = tool_input.get('skill', '')
        args = tool_input.get('args', '')
        target = f'skill={skill}' + (f' args={args}' if args else '')
    elif tool == 'WebFetch':
        url = tool_input.get('url', '')
        target = url
    elif tool == 'WebSearch':
        query = tool_input.get('query', '')
        target = f'query={query}'
    elif tool == 'LSP':
        op = tool_input.get('operation', '')
        file_path = tool_input.get('filePath', '')
        line = tool_input.get('line', '')
        target = f'op={op} file={file_path}:{line}'
    elif tool == 'AskUserQuestion':
        questions = tool_input.get('questions', [])
        if questions:
            headers = [q.get('header', '') for q in questions]
            target = f'questions={",".join(headers)}'
    elif tool == 'TodoWrite':
        todos = tool_input.get('todos', [])
        target = f'todos={len(todos)}'
    else:
        # For other tools, try to get a representative field
        target = str(tool_input)[:100]

    time = datetime.datetime.now().isoformat()
    with open('.tmp/hooked-sessions/tool-used.txt', 'a') as f:
        f.write(f'[{time}] [{session}] [{workflow}] {tool} | {target}\n')

# TODO: Add logic to check if tool usage should be blocked or allowed based on the session state later.
output = {
    "hookSpecificOutput": {
        "hookEventName": "PreToolUse",
        "permissionDecision": "allow"
    }
}
print(json.dumps(output))