"""A slightly higher-level AS2 assembler on top of gfxpatch: labels for jumps, and helpers for the
common shapes (calls, members, variables), so new menu code can be written readably.

    c = Code()
    c += call('GotoPage', 'OptionsScreen')          # GotoPage('OptionsScreen'), result dropped
    c += if_false(getvar('FGUYOnline'), 'skip')     # if (!FGUYOnline) goto skip
    c.label('skip')
    body = c.assemble()

Values passed to the helpers are either constants (str / int / bool / None / Reg) or Code fragments
that leave one value on the stack.
"""
import struct

from gfxpatch import Reg, action, push


class Code:
    def __init__(self, *parts):
        self.parts = []
        for p in parts:
            self += p

    def __iadd__(self, other):
        if isinstance(other, Code):
            self.parts += other.parts
        elif isinstance(other, bytes):
            self.parts.append(other)
        else:
            raise TypeError(other)
        return self

    def __add__(self, other):
        c = Code(self)
        c += other
        return c

    def label(self, name):
        self.parts.append(('label', name))
        return self

    def jump(self, name):
        self.parts.append(('Jump', name))
        return self

    def if_true(self, name):  # pops a value; jumps when it's true
        self.parts.append(('If', name))
        return self

    def assemble(self):
        offsets, at = {}, 0
        for p in self.parts:  # pass 1: label offsets (jumps are always 5 bytes)
            if isinstance(p, bytes):
                at += len(p)
            elif p[0] == 'label':
                offsets[p[1]] = at
            else:
                at += 5
        out, at = b'', 0
        for p in self.parts:
            if isinstance(p, bytes):
                out += p
                at += len(p)
            elif p[0] != 'label':
                at += 5
                out += action(p[0], struct.pack('<h', offsets[p[1]] - at))
        return out


def val(v):
    """A value onto the stack."""
    return v if isinstance(v, Code) else Code(push(v))


def pushes(*values):
    """Several values, in order (consecutive constants share one Push)."""
    c, run = Code(), []
    for v in values:
        if isinstance(v, Code):
            if run:
                c += push(*run)
                run = []
            c += v
        else:
            run.append(v)
    if run:
        c += push(*run)
    return c


def getvar(name):
    return Code(push(name), action('GetVariable'))


def setvar(name, value):
    return pushes(name, value) + action('SetVariable')


def member(obj, *names):
    """obj.a.b.c (names may be Code too)."""
    c = val(obj)
    for n in names:
        c += val(n) + action('GetMember')
    return c


def setmember(obj, name, value):
    return pushes(obj, name, value) + action('SetMember')


def callv(fn, *args):
    """fn(args) leaving the result on the stack."""
    return pushes(*reversed(args), len(args), fn) + action('CallFunction')


def call(fn, *args):
    return callv(fn, *args) + action('Pop')


def methodv(obj, name, *args):
    return pushes(*reversed(args), len(args), obj, name) + action('CallMethod')


def method(obj, name, *args):
    return methodv(obj, name, *args) + action('Pop')


def external(name, *args):
    """flash.external.ExternalInterface.call(name, args) leaving the result."""
    return methodv(member(getvar('flash'), 'external', 'ExternalInterface'), 'call', name, *args)


def new_object(cls, *args):
    return pushes(*reversed(args), len(args), cls) + action('NewObject')


def store(reg, value):
    return val(value) + action('StoreRegister', bytes([reg])) + action('Pop')


def op(name):
    return Code(action(name))


def define_function2(name, params, body, registers=8, flags=0x6a):
    """DefineFunction2 like the game's own menu functions: flags 0x6a preload _root into r1 (and
    suppress this/arguments/super); params go to r2, r3, ..."""
    first = 2 if flags & 0x40 else 1
    hdr = name.encode() + b'\0' + struct.pack('<HBH', len(params), registers, flags)
    for i, p in enumerate(params):
        hdr += bytes([first + i]) + p.encode() + b'\0'
    code = body.assemble() if isinstance(body, Code) else body
    hdr += struct.pack('<H', len(code))
    return action('DefineFunction2', hdr) + code


ROOT = Reg(1)
