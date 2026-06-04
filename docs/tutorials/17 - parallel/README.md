# Tutorial 17. Parallel actions

## What you're going to learn

In this tutorial you're going to learn about the `parallel` block in Testo-lang,
which lets you set up several virtual machines at the same time instead of one
after another.

## Introduction

Quite often a test requires a long preparation of several virtual machines: you
boot them, start some lengthy configuration script inside each guest and then
wait for it to finish. Written the usual (sequential) way, the test waits for
the first machine to finish before it even starts configuring the second one:

```testo
test long_setup {
	vm_server1 {
		start
		type "configure.sh"; press Enter
		wait "DONE" timeout 10m
	}
	vm_server2 {
		start
		type "configure.sh"; press Enter
		wait "DONE" timeout 10m
	}
	vm_client1 { start; type "configure.sh"; press Enter; wait "DONE" timeout 10m }
	vm_client2 { start; type "configure.sh"; press Enter; wait "DONE" timeout 10m }
}
```

If each `configure.sh` takes 10 minutes, the whole setup takes up to 40 minutes,
even though the four machines could just as well be configured at the same time.

People used to work around this by hand: first send all the `type`/`press`
commands to every machine, and only then collect all the `wait`s at the end. It
works, but it's verbose and error-prone — you have to interleave the commands
manually and keep them in sync.

## The `parallel` block

The `parallel` block does the interleaving for you. Every command inside the
block runs concurrently, and the block finishes only when **all** of its
commands have finished:

```testo
test long_setup {
	parallel {
		vm_server1 {
			start
			type "configure.sh"; press Enter
			wait "DONE" timeout 10m
		}
		vm_server2 {
			start
			type "configure.sh"; press Enter
			wait "DONE" timeout 10m
		}
		vm_client1 {
			start
			type "configure.sh"; press Enter
			wait "DONE" timeout 10m
		}
		vm_client2 {
			start
			type "configure.sh"; press Enter
			wait "DONE" timeout 10m
		}
	}
}
```

Now the four `configure.sh` scripts run at the same time, and the whole setup
takes about 10 minutes instead of 40.

### Getting rid of the copy-paste with a macro

Since all four branches do exactly the same thing, a command macro makes the
test much shorter. Note that the machine name is passed as a parameter, so the
same macro works for any machine:

```testo
macro configure(vmname) {
	"${vmname}" {
		start
		type "echo '${vmname} setup started'"; press Enter
		type "configure.sh"; press Enter
		wait "DONE" timeout 10m
	}
}

test long_setup {
	parallel {
		configure("vm_server1")
		configure("vm_server2")
		configure("vm_client1")
		configure("vm_client2")
	}
}
```

This is exactly the use case the `parallel` block was made for.

## How it works

Testo runs the whole test scenario on a single cooperative event loop, not on
operating-system threads. The blocking actions (`wait`, `sleep`, `exec`, network
IO, …) yield control while they are waiting. So when one branch is blocked in
`wait "DONE"`, the event loop is free to run the other branches. The heavy work
(`configure.sh`) runs *inside* the guests anyway — the `parallel` block simply
lets Testo wait for all of them at once.

Because everything runs on one cooperative loop (no preemption), there are no
data races to worry about: branches only ever switch at well-defined yield
points.

## Things to keep in mind

- **One machine per branch.** Two branches of the same `parallel` block must not
  touch the same virtual machine — typing into the same guest from two branches
  at once would interleave the keystrokes and produce garbage. Give every branch
  its own machine.
- **Interleaved output.** The progress lines of the branches are printed as they
  happen, so messages from different machines will be mixed together in the log.
  Each line is still prefixed with the name of the machine it belongs to.
- **Fail-fast.** If one branch fails, the remaining branches are cancelled and
  the whole `parallel` block (and the test) fails.
- **Nesting.** `parallel` blocks may be nested if you ever need a more elaborate
  fan-out, although a single level is enough for the vast majority of cases.

## Conclusion

The `parallel` block is pure syntactic sugar over the fan-out/fan-in pattern
that you could write by hand — but it keeps the test readable and lets Testo do
the bookkeeping. Use it whenever you have several independent machines that need
a long, independent setup.
