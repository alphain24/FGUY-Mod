"""A small compiler from a JavaScript-like language to ActionScript 2 bytecode (DefineFunction2 blocks),
for writing new menu code (src/ui/*.as2js) that gets patched into the game's Scaleform movies.

Supported:
  top level      function name(a, b) { ... }        (becomes a timeline function of the movie)
  statements     var x = e, y;   if / else   while   for (init; cond; step)   break   continue
                 return [e];   expression statements;   { blocks }
  expressions    numbers, 'strings' / "strings", true false null undefined this _root
                 a.b  a[b]  f(x)  o.m(x)  new C(x)  new a.B(x)  [1, 2]  {k: v, 'k2': v}
                 function (a) { ... }               (no access to the outer function's locals)
                 = += -= *= /=   ?:   || &&   == != === !==   < > <= >=   + - * / %
                 ! - typeof (prefix)   ++ -- (prefix / postfix)
Names: params and `var`s are registers; `this` and `_root` are preloaded registers; anything else is
a timeline variable (GetVariable / SetVariable), as in the game's own scripts.

Usage as a library: compile_source(text) -> bytes (all top-level functions, concatenated).
Self-test: python as2c.py --test
"""
import re
import struct
import sys

from gfxpatch import action

# ---- tokens ---------------------------------------------------------------------------------------
TOKEN = re.compile(r'''
    (?P<ws>\s+|//[^\n]*|/\*.*?\*/)
  | (?P<num>0x[0-9a-fA-F]+|\d+\.\d*|\.\d+|\d+)
  | (?P<str>'(?:[^'\\]|\\.)*'|"(?:[^"\\]|\\.)*")
  | (?P<name>[A-Za-z_$][A-Za-z0-9_$]*)
  | (?P<op>===|!==|==|!=|<=|>=|&&|\|\||\+\+|--|\+=|-=|\*=|/=|[{}()\[\];,.?:+\-*/%<>!=])
''', re.S | re.X)
KEYWORDS = {'function', 'var', 'if', 'else', 'while', 'for', 'return', 'break', 'continue', 'new', 'typeof',
            'true', 'false', 'null', 'undefined', 'this'}


class Tok:
    def __init__(self, kind, value, line):
        self.kind, self.value, self.line = kind, value, line

    def __repr__(self):
        return f'{self.kind}:{self.value}'


def tokenize(src):
    out, pos, line = [], 0, 1
    while pos < len(src):
        m = TOKEN.match(src, pos)
        if not m:
            raise SyntaxError(f'line {line}: unexpected {src[pos:pos + 20]!r}')
        kind = m.lastgroup
        text = m.group()
        if kind != 'ws':
            if kind == 'name' and text in KEYWORDS:
                kind = 'kw'
            if kind == 'str':
                text = bytes(text[1:-1], 'latin1').decode('unicode_escape')
            out.append(Tok(kind, text, line))
        line += m.group().count('\n')
        pos = m.end()
    out.append(Tok('eof', None, line))
    return out


# ---- syntax tree (tuples) -------------------------------------------------------------------------
class Parser:
    def __init__(self, toks):
        self.t, self.i = toks, 0

    def peek(self, k=0):
        return self.t[self.i + k]

    def at(self, value, kind=None):
        t = self.peek()
        return t.value == value and (kind is None or t.kind == kind) and t.kind in ('op', 'kw')

    def take(self, value=None, kind=None):
        t = self.peek()
        if value is not None and not (t.value == value and t.kind in ('op', 'kw')):
            raise SyntaxError(f'line {t.line}: expected {value!r}, got {t.value!r}')
        if kind is not None and t.kind != kind:
            raise SyntaxError(f'line {t.line}: expected a {kind}, got {t.value!r}')
        self.i += 1
        return t

    def program(self):
        funcs = []
        while self.peek().kind != 'eof':
            funcs.append(self.function(named=True))
        return funcs

    def function(self, named):
        line = self.take('function').line
        name = self.take(kind='name').value if named else ''
        self.take('(')
        params = []
        while not self.at(')'):
            params.append(self.take(kind='name').value)
            if not self.at(')'):
                self.take(',')
        self.take(')')
        return ('function', name, params, self.block(), line)

    def block(self):
        self.take('{')
        body = []
        while not self.at('}'):
            body.append(self.statement())
        self.take('}')
        return ('block', body)

    def statement(self):
        t = self.peek()
        if self.at('{'):
            return self.block()
        if self.at(';'):
            self.take(';')
            return ('block', [])
        if self.at('var', 'kw'):
            s = self.var_decl()
            self.take(';')
            return s
        if self.at('if', 'kw'):
            self.take('if'); self.take('(')
            cond = self.expr()
            self.take(')')
            then = self.statement()
            other = None
            if self.at('else', 'kw'):
                self.take('else')
                other = self.statement()
            return ('if', cond, then, other)
        if self.at('while', 'kw'):
            self.take('while'); self.take('(')
            cond = self.expr()
            self.take(')')
            return ('for', None, cond, None, self.statement())
        if self.at('for', 'kw'):
            self.take('for'); self.take('(')
            init = None if self.at(';') else (self.var_decl() if self.at('var', 'kw') else ('expr', self.expr()))
            self.take(';')
            cond = None if self.at(';') else self.expr()
            self.take(';')
            step = None if self.at(')') else ('expr', self.expr())
            self.take(')')
            return ('for', init, cond, step, self.statement())
        if self.at('return', 'kw'):
            self.take('return')
            value = None if self.at(';') else self.expr()
            self.take(';')
            return ('return', value)
        if self.at('break', 'kw') or self.at('continue', 'kw'):
            word = self.take().value
            self.take(';')
            return (word,)
        e = self.expr()
        self.take(';')
        return ('expr', e)

    def var_decl(self):
        self.take('var')
        decls = []
        while True:
            name = self.take(kind='name').value
            value = None
            if self.at('='):
                self.take('=')
                value = self.assign()
            decls.append((name, value))
            if not self.at(','):
                return ('var', decls)
            self.take(',')

    # expressions, lowest precedence first
    def expr(self):
        return self.assign()

    def assign(self):
        left = self.ternary()
        for op in ('=', '+=', '-=', '*=', '/='):
            if self.at(op, 'op'):
                self.take(op)
                if left[0] not in ('name', 'member'):
                    raise SyntaxError(f'line {self.peek().line}: can only assign to a name or a member')
                return ('assign', op, left, self.assign())
        return left

    def ternary(self):
        cond = self.binary(0)
        if self.at('?'):
            self.take('?')
            a = self.assign()
            self.take(':')
            return ('cond', cond, a, self.assign())
        return cond

    LEVELS = [('||',), ('&&',), ('==', '!=', '===', '!=='), ('<', '>', '<=', '>='), ('+', '-'), ('*', '/', '%')]

    def binary(self, level):
        if level == len(self.LEVELS):
            return self.unary()
        left = self.binary(level + 1)
        while self.peek().kind == 'op' and self.peek().value in self.LEVELS[level]:
            op = self.take().value
            right = self.binary(level + 1)
            left = ('logic' if op in ('&&', '||') else 'bin', op, left, right)
        return left

    def unary(self):
        if self.at('!') or self.at('-'):
            op = self.take().value
            return ('unary', op, self.unary())
        if self.at('typeof', 'kw'):
            self.take()
            return ('unary', 'typeof', self.unary())
        if self.at('++') or self.at('--'):
            op = self.take().value
            return ('incdec', op, True, self.postfix())
        e = self.postfix()
        if self.at('++') or self.at('--'):
            return ('incdec', self.take().value, False, e)
        return e

    def postfix(self):
        if self.at('new', 'kw'):
            self.take('new')
            target = self.primary()
            while self.at('.'):
                self.take('.')
                target = ('member', target, ('str', self.take(kind='name').value))
            args = self.args() if self.at('(') else []
            e = ('new', target, args)
        else:
            e = self.primary()
        while True:
            if self.at('.'):
                self.take('.')
                e = ('member', e, ('str', self.take(kind='name').value))
            elif self.at('['):
                self.take('[')
                k = self.expr()
                self.take(']')
                e = ('member', e, k)
            elif self.at('('):
                e = ('call', e, self.args())
            else:
                return e

    def args(self):
        self.take('(')
        out = []
        while not self.at(')'):
            out.append(self.assign())
            if not self.at(')'):
                self.take(',')
        self.take(')')
        return out

    def primary(self):
        t = self.peek()
        if t.kind == 'num':
            self.take()
            v = int(t.value, 16) if t.value.startswith('0x') else (float(t.value) if '.' in t.value else int(t.value))
            return ('num', v)
        if t.kind == 'str':
            self.take()
            return ('str', t.value)
        if t.kind == 'name':
            self.take()
            return ('name', t.value)
        if t.kind == 'kw' and t.value in ('true', 'false', 'null', 'undefined', 'this'):
            self.take()
            return ('const', t.value)
        if self.at('function', 'kw'):
            return ('func', self.function(named=False))
        if self.at('('):
            self.take('(')
            e = self.expr()
            self.take(')')
            return e
        if self.at('['):
            self.take('[')
            items = []
            while not self.at(']'):
                items.append(self.assign())
                if not self.at(']'):
                    self.take(',')
            self.take(']')
            return ('array', items)
        if self.at('{'):
            self.take('{')
            pairs = []
            while not self.at('}'):
                k = self.take()
                if k.kind not in ('name', 'str', 'kw'):
                    raise SyntaxError(f'line {k.line}: bad object key {k.value!r}')
                self.take(':')
                pairs.append((k.value, self.assign()))
                if not self.at('}'):
                    self.take(',')
            self.take('}')
            return ('object', pairs)
        raise SyntaxError(f'line {t.line}: unexpected {t.value!r}')


# ---- code generation --------------------------------------------------------------------------------
def push_value(v):
    """One Push record (type byte + data) for a constant."""
    if v is True or v is False:
        return b'\x05' + bytes([1 if v else 0])
    if v is None:
        return b'\x02'  # null
    if isinstance(v, str):
        return b'\x00' + v.encode('latin1') + b'\0'
    if isinstance(v, int) and -2 ** 31 <= v < 2 ** 31:
        return b'\x07' + struct.pack('<i', v)
    d = struct.pack('<d', float(v))
    return b'\x06' + d[4:] + d[:4]  # SWF doubles: high word first


UNDEF = object()


class Fn:
    """Code generator for one function body."""
    FLAGS = 0x69  # preload this -> r1, _root -> r2; suppress arguments and super

    def __init__(self, params):
        self.regs = {'this': 1, '_root': 2}
        for p in params:
            self.regs[p] = len(self.regs) + 1
        self.parts = []      # bytes, or ('label', n) / ('Jump'|'If', n)
        self.nlabels = 0
        self.loops = []      # (continue label, break label)

    def label(self):
        self.nlabels += 1
        return self.nlabels

    def emit(self, b):
        self.parts.append(b)

    def mark(self, n):
        self.parts.append(('label', n))

    def jump(self, kind, n):
        self.parts.append((kind, n))

    def push(self, *vals):
        body = b''
        for v in vals:
            if v is UNDEF:
                body += b'\x03'
            elif isinstance(v, tuple) and v[0] == 'reg':
                body += b'\x04' + bytes([v[1]])
            else:
                body += push_value(v)
        self.emit(action('Push', body))

    def op(self, name, body=b''):
        self.emit(action(name, body))

    def reg_for(self, name):
        return self.regs.get(name)

    def declare(self, name):
        if name not in self.regs:
            if len(self.regs) >= 250:
                raise SyntaxError('too many local variables')
            self.regs[name] = len(self.regs) + 1
        return self.regs[name]

    def assemble(self):
        at, where = 0, {}
        for p in self.parts:
            if isinstance(p, bytes):
                at += len(p)
            elif p[0] == 'label':
                where[p[1]] = at
            else:
                at += 5
        out, at = b'', 0
        for p in self.parts:
            if isinstance(p, bytes):
                out += p
                at += len(p)
            elif p[0] != 'label':
                at += 5
                out += action(p[0], struct.pack('<h', where[p[1]] - at))
        return out


def collect_vars(node, out):
    """Every `var` name in a function body (not inside nested functions)."""
    if isinstance(node, tuple):
        if node and node[0] == 'var':
            out.extend(name for name, _ in node[1])
        if node and node[0] == 'func':
            return
        for x in node:
            collect_vars(x, out)
    elif isinstance(node, list):
        for x in node:
            collect_vars(x, out)


def compile_function(fnode):
    _, name, params, body, line = fnode
    f = Fn(params)
    names = []
    collect_vars(body, names)
    for n in names:
        f.declare(n)
    stmt(f, body)
    code = f.assemble()
    nregs = len(f.regs) + 1
    hdr = name.encode('latin1') + b'\0' + struct.pack('<HBH', len(params), nregs, Fn.FLAGS)
    for p in params:
        hdr += bytes([f.regs[p]]) + p.encode('latin1') + b'\0'
    hdr += struct.pack('<H', len(code))
    if len(code) > 0xffff:
        raise SyntaxError(f'function {name} is too big')
    return action('DefineFunction2', hdr) + code


def stmt(f, s):
    k = s[0]
    if k == 'block':
        for x in s[1]:
            stmt(f, x)
    elif k == 'var':
        for name, value in s[1]:
            if value is not None:
                expr(f, value)
                f.op('StoreRegister', bytes([f.regs[name]]))
                f.op('Pop')
    elif k == 'expr':
        expr(f, s[1], want=False)
    elif k == 'if':
        other, end = f.label(), f.label()
        expr(f, s[1])
        f.op('Not')
        f.jump('If', other)
        stmt(f, s[2])
        if s[3] is not None:
            f.jump('Jump', end)
        f.mark(other)
        if s[3] is not None:
            stmt(f, s[3])
            f.mark(end)
    elif k == 'for':
        _, init, cond, step, body = s
        if init is not None:
            stmt(f, init)
        top, cont, end = f.label(), f.label(), f.label()
        f.mark(top)
        if cond is not None:
            expr(f, cond)
            f.op('Not')
            f.jump('If', end)
        f.loops.append((cont, end))
        stmt(f, body)
        f.loops.pop()
        f.mark(cont)
        if step is not None:
            stmt(f, step)
        f.jump('Jump', top)
        f.mark(end)
    elif k == 'return':
        if s[1] is None:
            f.push(UNDEF)
        else:
            expr(f, s[1])
        f.op('Return')
    elif k in ('break', 'continue'):
        if not f.loops:
            raise SyntaxError(f'{k} outside a loop')
        f.jump('Jump', f.loops[-1][1 if k == 'break' else 0])
    else:
        raise SyntaxError(f'unknown statement {k}')


BIN = {'+': ['Add2'], '-': ['Subtract'], '*': ['Multiply'], '/': ['Divide'], '%': ['Modulo'],
       '==': ['Equals2'], '!=': ['Equals2', 'Not'], '===': ['StrictEquals'], '!==': ['StrictEquals', 'Not'],
       '<': ['Less2'], '>': ['Greater'], '<=': ['Greater', 'Not'], '>=': ['Less2', 'Not']}


def expr(f, e, want=True):
    """Leaves e's value on the stack (want=False: leaves nothing)."""
    k = e[0]
    if k == 'assign':
        assign(f, e, want)
        return
    if k == 'incdec':
        _, op, prefix, target = e
        delta = ('num', 1)
        assign(f, ('assign', '+=' if op == '++' else '-=', target, delta), want, postfix_old=want and not prefix)
        return
    if k == 'call':
        call(f, e)
    elif k == 'num':
        f.push(e[1])
    elif k == 'str':
        f.push(e[1])
    elif k == 'const':
        v = e[1]
        if v == 'this':
            f.push(('reg', 1))
        else:
            f.push({'true': True, 'false': False, 'null': None, 'undefined': UNDEF}[v])
    elif k == 'name':
        r = f.reg_for(e[1])
        if r:
            f.push(('reg', r))
        else:
            f.push(e[1])
            f.op('GetVariable')
    elif k == 'member':
        expr(f, e[1])
        expr(f, e[2])
        f.op('GetMember')
    elif k == 'bin':
        expr(f, e[2])
        expr(f, e[3])
        for a in BIN[e[1]]:
            f.op(a)
    elif k == 'logic':
        end = f.label()
        expr(f, e[2])
        f.op('PushDuplicate')
        if e[1] == '&&':
            f.op('Not')
        f.jump('If', end)
        f.op('Pop')
        expr(f, e[3])
        f.mark(end)
    elif k == 'unary':
        if e[1] == '-':
            f.push(0)
            expr(f, e[2])
            f.op('Subtract')
        else:
            expr(f, e[2])
            f.op('Not' if e[1] == '!' else 'TypeOf')
    elif k == 'cond':
        yes, end = f.label(), f.label()
        expr(f, e[1])
        f.jump('If', yes)
        expr(f, e[3])
        f.jump('Jump', end)
        f.mark(yes)
        expr(f, e[2])
        f.mark(end)
    elif k == 'array':
        for item in reversed(e[1]):
            expr(f, item)
        f.push(len(e[1]))
        f.op('InitArray')
    elif k == 'object':
        for key, value in e[1]:
            f.push(key)
            expr(f, value)
        f.push(len(e[1]))
        f.op('InitObject')
    elif k == 'new':
        _, target, args = e
        for a in reversed(args):
            expr(f, a)
        f.push(len(args))
        if target[0] == 'name' and not f.reg_for(target[1]):
            f.push(target[1])
            f.op('NewObject')
        elif target[0] == 'member':
            expr(f, target[1])
            expr(f, target[2])
            f.op('NewMethod')
        else:
            expr(f, target)
            f.push(UNDEF)
            f.op('NewMethod')
    elif k == 'func':
        inner = e[1]
        check_no_outer_locals(inner, f)
        f.emit(compile_function(inner))
    else:
        raise SyntaxError(f'unknown expression {k}')
    if not want:
        f.op('Pop')


def check_no_outer_locals(fnode, outer):
    _, _, params, body, line = fnode
    own = set(params) | {'this', '_root'}
    names = []
    collect_vars(body, names)
    own |= set(names)

    def walk(n):
        if isinstance(n, tuple):
            if n and n[0] == 'name' and n[1] not in own and n[1] in outer.regs:
                raise SyntaxError(f'line {line}: a nested function can\'t use the outer local "{n[1]}" '
                                  f'(store it on an object or a timeline variable)')
            if n and n[0] == 'func':
                return
            for x in n:
                walk(x)
        elif isinstance(n, list):
            for x in n:
                walk(x)
    walk(body)


def call(f, e):
    _, target, args = e
    if target[0] == 'name' and target[1] == 'getURL' and not f.reg_for('getURL'):  # an instruction (so is getTimer)
        expr(f, args[0])
        expr(f, args[1] if len(args) > 1 else ('str', ''))
        f.op('GetURL2', b'\x00')
        f.push(UNDEF)  # it has no value; calls always leave one
        return
    if target[0] == 'name' and target[1] == 'getTimer' and not args and not f.reg_for('getTimer'):  # an instruction
        f.op('GetTime')
        return
    for a in reversed(args):
        expr(f, a)
    f.push(len(args))
    if target[0] == 'name' and not f.reg_for(target[1]):
        f.push(target[1])
        f.op('CallFunction')
    elif target[0] == 'member':
        expr(f, target[1])
        expr(f, target[2])
        f.op('CallMethod')
    else:  # a function value: call it with no method name
        expr(f, target)
        f.push(UNDEF)
        f.op('CallMethod')


def assign(f, e, want, postfix_old=False):
    _, op, target, value = e

    def rhs():
        if op != '=':
            expr(f, target)
            if postfix_old:
                f.op('StoreRegister', b'\x00')  # old value, kept for the result
                f.op('Pop')
            expr(f, value)
            f.op(BIN[op[0]][0])
        else:
            expr(f, value)

    if target[0] == 'name':
        r = f.reg_for(target[1])
        if r:
            rhs()
            f.op('StoreRegister', bytes([r]))
            if not want:
                f.op('Pop')
            elif postfix_old:
                f.op('Pop')
                f.push(('reg', 0))
            return
        f.push(target[1])
        rhs()
        if want and not postfix_old:
            f.op('StoreRegister', b'\x00')
        f.op('SetVariable')
    else:  # member
        expr(f, target[1])
        expr(f, target[2])
        rhs()
        if want and not postfix_old:
            f.op('StoreRegister', b'\x00')
        f.op('SetMember')
    if want:
        f.push(('reg', 0))


def compile_source(src):
    return b''.join(compile_function(fn) for fn in Parser(tokenize(src)).program())


# ---- self-test ----------------------------------------------------------------------------------------
TEST = r'''
function Sum(list) {
    var total = 0;
    for (var i = 0; i < list.length; i++) { if (list[i] == null) continue; total += list[i]; }
    return total;
}
function Demo(a, b) {
    var o = {name: 'x', n: 2.5}, arr = [1, 'two', true];
    o.n = o.n * 2 - -1;
    _root.FGUYTest = a && b || !a ? Sum(arr) : 0;
    var mc = _root.createEmptyMovieClip('t', 99);
    mc.onRelease = function () { this._alpha = 50; };
    var fmt = new TextFormat('$font_bodyText', 18, 0xFFFFFF);
    while (a > 0) { a--; if (a == 3) break; }
    return typeof o + String(arr.length);
}
'''

if __name__ == '__main__':
    if '--test' in sys.argv:
        import os
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 're'))
        import as2
        code = compile_source(TEST)
        print(f'{len(code)} bytes')
        print('\n'.join(as2.disasm(code, 0, len(code))))
