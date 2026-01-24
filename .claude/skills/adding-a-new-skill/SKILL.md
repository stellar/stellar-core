---
name: adding-a-skill
description: extending yourself with a new reusable skill by interviewing the user
---

If the user wants to add a new skill, you can help them with this:

1. Ask the user for a short name and description of the skill. The name should be
   something that can be rendered as a short (10-30 character) descriptive sequence
   of words separated by hyphens. The description should be a line of text that is
   focused on conveying to a future agent when the skill would be appropriate to
   use and roughly what it does.

2. Ask the user for details. You can build up an idea of what the skill should do
   over multiple rounds of questioning. You want to find out:

    - If the skill should involve any key thoughts or considerations.
    - If the skill has an order of steps, or is an unordered set of tasks.
    - If the skill involves running sub-agents and if so how many.
    - **If the skill involves a lot of work** that might benefit from splitting
      into subagent tasks (see below for considerations).
    - If the skill should include commands to run.
    - If the skill should include code to write.
    - If for code or commands to there is specific text or more of a
      sketch or template of text _like_ some example to include. 
    - Any hard rules that an agent doing the skill should ALWAYS or NEVER do.
    - A set of conditions for stopping, looping/extending, or resetting/restarting.
    - How the result of applying the skill should be conveyed to the user.

   **Subagent considerations**
   
   Skills that involve a lot of work may benefit from splitting into pieces and
   running each piece as a subagent. This keeps each subagent focused on a
   moderate amount of work so it doesn't get lost or wander off track. If the
   skill might use subagents, identify:
   
    - How the work could be split into moderate-sized pieces
    - What information each subagent needs to do its piece
    - The skill file should have an "Inputs" section listing what's needed
    - The skill should suggest a format for the subagent prompt 

3. Once you have learned this information from the user, assemble it into a
   file in the repository. Add a file named `.claude/skills/<skill-name>/SKILL.md`
   with the following structure:

    - YAML frontmatter with the fields `name` and `description` drawn from
      the name and description.
    - An introductory paragraph or two describing the goal of the skill and
      any thoughts or special considerations to perform, as well as any
      description of the meta-parameters like how to split work among subagents
      and how to stop/loop/restart. If the skill involves a lot of work,
      suggest how it might be split into moderate-sized subagent tasks.
    - **If the skill might use subagents**: An "Inputs" section that lists
      what information is needed for each piece, and suggests a format for the
      subagent prompt. This section comes right after the overview.
    - Either a sequential list of steps or an unordered list of tasks.
    - Any code or commands in either specific or example form.
    - Any of the ALWAYS/NEVER conditions.
    - A "Completion" section describing how to summarize the work, noting that
      if invoked as a subagent, the summary should be passed back to the
      invoking agent.

When you're done, save that file and then present the user with a link to it
so they can open it and review it.