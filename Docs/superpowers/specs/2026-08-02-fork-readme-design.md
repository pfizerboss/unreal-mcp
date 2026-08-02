# Fork README Design

## Goal

Replace the long upstream-oriented root README with a concise bilingual landing page for `pfizerboss/unreal-mcp`, focused on what this fork adds to the original `GenOrca/unreal-mcp` project.

## Audience and language

The README targets Unreal Engine developers who want an MCP server with broad, safe Blueprint authoring support. Russian is the primary language; a compact English version follows with the same facts and no additional promises.

## Structure

1. Project title and one-sentence fork positioning.
2. Russian overview.
3. Compact comparison: original baseline versus this fork.
4. Added functionality grouped into Blueprint 2.0, native palette and semantic editing, diagnostics, transactional workflows, and LLM-oriented MCP contracts.
5. Three short Blueprint workflow examples: connected spawn, insertion, and replacement.
6. Minimal installation and MCP client configuration.
7. Verified compatibility and test status.
8. English summary mirroring the Russian content.
9. Attribution to the original project and Apache 2.0 license.

## Facts to preserve

- The original baseline had 253 actions across 21 domains, including 19 Blueprint actions.
- This fork has 299 actions across 22 domains, including 57 Blueprint actions.
- The fork adds 46 public actions, 38 of them in the Blueprint domain.
- UE 5.7 is the locally verified release target.
- UE 5.6 was not tested because it is not installed.
- UE 5.8 was not tested because the local installation lacks build tools, editor binaries, and source headers.
- Blueprint mutations do not compile or save implicitly.
- The base-game generator is outside the project scope.

## Content boundaries

Remove the upstream release badges, Fab and video promotion, full 299-action catalog, long extension guide, and verbose troubleshooting material from the root README. Do not remove repository files or server documentation. Do not claim official affiliation with the original project or compatibility that was not locally verified.

## Presentation

Use compact Markdown, one comparison table, short bullet lists, and copyable command/configuration blocks. Avoid duplicated technical detail between the Russian and English sections. Keep the root README short enough to scan in a few minutes while still allowing a new user to understand the fork, install it, and follow the intended Blueprint workflow.

## Validation

- Confirm all action and domain counts against the generated catalog.
- Confirm every referenced path and command exists in the repository.
- Check Markdown headings and fenced blocks mechanically.
- Review the final diff for stale upstream release links and unsupported compatibility claims.
