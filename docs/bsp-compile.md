# Compiling a map to .bsp

`file > compile to .bsp...` in the editor. The game plays `.vmf` files
directly, so you only need this to put a map somewhere that wants a compiled
one -- a real CS:S server, or anyone else's copy of the game.

There are two compilers behind that menu item, and the window says which one it
is about to use.

## vbsp / vvis / vrad (what you want)

Valve's own tools, the same ones Hammer drives. A proper BSP tree, real
visibility and baked lighting: a map that behaves on a server like any other.

The editor looks for them in, in order:

1. the folder you picked with **compile tools...** (remembered in `content.cfg`)
2. `<your CS:S folder>/bin`
3. `Source SDK Base 2013 Multiplayer/bin` and the singleplayer one, beside CS:S
   in `steamapps/common`
4. `SourceSDK/bin/source2013/bin`, and the older `orangebox` path

It needs a `gameinfo.txt` to pass as `-game`, which it takes from your CS:S
folder (`cstrike/`), so set **content > cs:s folder...** first.

On Linux the tools are Windows executables, so they are run through `wine`. If
wine is not installed the window says so rather than failing halfway through a
compile.

**run vvis** and **run vrad** can each be turned off, and **fast** passes
`-fast` to both. Turning vvis off while you iterate takes the slowest stage out
of the loop; a map compiled without it still runs, it is just not optimised.

## The built-in writer (the fallback)

Used when the tools are not installed, and whenever you pick it. It needs
nothing installed and produces a map this game loads and plays.

What it does not do:

- **No visibility.** vvis's job has no cheap stand-in, so the map is one leaf
  in one cluster that always sees itself. Correct, but the engine draws the
  whole map every frame instead of only what you can see.
- **No baked lighting.** vrad's job. Every surface gets a flat fullbright
  lightmap, so there are no shadows and no light_environment colouring.
- **No physics collide lump.** vphysics props would not collide with the world
  in the real engine. Player movement is unaffected -- that uses the brushes,
  which are written properly.

So it is good for getting a playable map out of the editor quickly, and for
sharing a course with someone who just wants to run it. For a map going on a
server, compile it with Valve's tools.

It also refuses rather than writing a broken map when one runs past a limit of
the format: several BSP fields are 16-bit, and vbsp answers that by splitting
faces and brushes up, which this does not do. The message names whichever ran
out.

## Where the files go

The compile works from its own `<name>.compile.vmf` beside the output rather
than from your map, so it can never overwrite the `.vmf` you are editing. That
working copy and the portal file vvis leaves next to it are removed once the
compile succeeds, and kept when it fails, where they are worth reading.

## How much of this is tested

The writer is checked by round-tripping through the game's own loaders: a map
written from a document comes back with the same triangle count, material
groups, spawn point, triggers and brush count as playing the same `.vmf`
directly, and a shipped CS:S map decompiled and written back out reloads with
its geometry and collision intact. Its lump layout, lump versions and face
winding were all taken from a shipped CS:S map rather than from memory.

What is **not** tested is either compiler's output in the actual Source engine,
because there was no CS:S install in the environment this was built in. The
Valve-tools path is checked end to end against stub tools -- discovery,
argument passing, streamed output, the finished map moved into place -- and
vbsp's own output is vbsp's business. But if you are the first to put a
built-in-writer map on a real server, treat it as untried.
