// This module only maps events. It does not register a global keyboard hook.
export function actionForKey(event, keys, focused = true) {
  if (!focused || event.repeat || event.isComposing || event.ctrlKey || event.altKey || event.metaKey || event.shiftKey) return null;
  const target = event.target;
  if (target?.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(target?.tagName ?? '')) return null;
  for (const [action, key] of Object.entries(keys)) {
    if (event.key === key) return action;
  }
  return null;
}
