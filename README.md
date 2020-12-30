![chatra_logo](https://raw.githubusercontent.com/chatra-lang/resources/master/logo/chatra_logo_for_light_bg_small.png "Chatra")

# Programing language "Chatra"

Chatra is a lightweight scripting language which has simple design, but powerful.

The key concepts of Chatra is "easy to use".


## Getting started

At present, Chatra is provided in the form of the source codes written in C++11.

To build console frontend:

- Install cmake and C++11 compiler as you like
- Install readline library (`apt install libreadline6-dev` or `brew install readline`, etc)
- `mkdir build && cd build`
- `cmake ..` or with appropriate options
- `make`

To embed into your program:

- Copy the directories "chatra", "chatra_core", and "chatra_emb" (if required - this one contains embedded packages for Chatra scripts)
- Add "chatra" directory into include path
- Compile all "*.cpp" files and link them with your program, or use "CMakeLists.txt"


## Key features

### Simple & powerful

Chatra has very simple language structure.
If you are familiar with at least one programming language, you can understand chatra's script what it does.

```Text
class HelloWorld
    def someMethod(to: 'Earth')
        log('Hello,' + to + '!')

instance = HelloWorld()
for in Range(5)
    instance.someMethod('World')
```

Although it is simple, Chatra has many features which a modern programing language should have:

- Classes
- Scopes
- Exceptions
- Function overloading
- Operator overloading
- Function/method reference
- Closure
- Concurrent garbage collection
- Incremental parser

Chatra's grammar is influenced by well-known languages such as
C/C++, Java, Python and Swift.
If you have learned these languages, you can use Chatra immediately.


### Embeddable

Chatra's interpreter is provided as stand-alone library (only depends on C++11 runtime) and 
can be called from your program with simple steps:

```C++
// Generate Runtime and execute "test_script".
auto host = std::make_shared<Host>();
auto runtime = cha::Runtime::newInstance(host); 
auto packageId = runtime->loadPackage(cha::Script("test_script", "..."));
runtime->run(packageId);
```

You can export any native function to chatra's interpreter in this way:

```Text
def add10(a: Int) as native
```

```C++
// equivalent to:
// def add10(a)
//     return a + 10
static void someNativeFunction(cha::Ct& ct) {
    ct.set(ct[0].get<int>() + 10);
}

cha::PackageInfo getANativePackage() {
	std::vector<cha::Script> scripts = {{packageName, script}};
	std::vector<cha::HandlerInfo> handlers = {
			{someNativeFunction, "add10"}
	};
	return {scripts, handlers, nullptr};
}
```


### Portable

Chatra's interpreter is written with "almost pure" C++11 and only depends on C++11 standard library (prefixed "std").
The interpreter can be compiled and run on many platforms such as:

| OS | CPU arch | compiler |
|---|---|---|
|Linux (Ubuntu 16.04)|x86_64|g++ (Ubuntu 5.4.0-6ubuntu1~16.04.12) 5.4.0 20160609|
|Linux (Ubuntu 16.04)|x86_64|clang version 3.8.0-2ubuntu4 (tags/RELEASE_380/final)|
|macOS 11.1|x86_64|g++ (Homebrew GCC 9.2.0_1) 9.2.0|
|macOS 11.1|x86_64|Apple clang version 11.0.0 (clang-1100.0.33.12)|


### Serializable

An instance of chatra's interpreter (called `Runtime`) can be serialized into byte-stream,
and can be restored and resumed later.
The serialized byte-stream contains entire state of `Runtime`, including internal state of native libraries such as file pointers
in so far as these libraries provide serialize/de-serialize interfaces via `chatra::IPackage`.

```C++
auto host = std::make_shared<Host>();
auto runtime1 = cha::Runtime::newInstance(host); 
auto packageId = runtime1->loadPackage(cha::Script("test_script", "..."));
runtime1->run(packageId);

// … running script ...

// Save the runtime state to stream and shut it down.
std::vector<uint8_t> stream = runtime1->shutdown(true);

// Restore the Runtime from the saved stream.
runtime2 = cha::Runtime::newInstance(host, stream);

// … resume execution ...
```

This feature is useful in situations where frequent stop/resume occurs,
such as smartphone apps.  
If the conditions are met, it can even be resumed on another machine
(strictly saying, there are some prerequisites).


### Supports Threading

Chatra supports multi-threading in language level.
Calling a function with `async` keyword indicates it will be invoked in another thread so 
the caller's thread can process another work simultaneously.

```Text
def someFunction()
    i: 0
    for j in Range(10000)
        i += j
    return i

a = async someFunction()
// do some other works...
wait(a)
log('sum=' + a.result)
```


### Reference-based mutual exclusion

The mutual exclusion is one of the most troublesome topics in multi-threading.
Chatra has a reference-oriented mutual exclusion mechanism to keep implementation simple which has features of:

- Any reference (variables and class members) can be owned by only one thread at the same time.
- Ownership to references are exchanged at the timing of function-call/return boundary.
- References grouped by `sync` block are only take one owner;
  This means, while one of thread manipulates some references in 'sync' block, all of other threads will be blocked when attempt to access any references inside the `sync` block.
- Additionally, ownership to a `sync` block is never released until its owned function returns.

```Text
sync
    var a: 0

def yield()

def interference()
    a = 1  // sync block is locked
    yield()
    a = 2
    // lock released

def interferenceThread()
    while true
        interference()

t = async interferenceThread()
// variable "a" is always observed as 2 (never be 1)
```


### Debugger

The chatra interpreter has an interface for debugging, called `IDebugger`.
Also, the console frontend of chatra supports all debugging features.

```ShellSession
Chatra interactive frontend
type "!h" to show debugger command help
chatra[0]:1> !run "debug_test.cha"
[I3] P2 file: debug_test.cha
chatra[0]:1> !b @debug_test(8)
chatra[0]:1> !resume
a=123
b=234
[reached to breakpoint B0]
  B0 @debug_test.cha(8): return b + c
chatra[0]:1> !threads
[T3] I3, primary=P2, 3frames:{T3:F10 type=Method, T3:F6 type=Method, T3:F3 type=ScriptRoot} @debug_test.cha(8): return b + c
[T2] I2, primary=P1, 0frames:{} @unknown
chatra[0]:1> !step over T3
[paused] @debug_test.cha(11): log('func3(b)=' + func4(b))
```


## Why called "chatra" ?

The name of “Chatra” comes from the Japanese “red tabby”. 
Red tabbies are simple and friendly, 
so their characteristics is suitable for this language.

![red_tabby](https://raw.githubusercontent.com/chatra-lang/resources/master/images/red_tabby.jpg "Chatra \(red tabby\) is simple")
![calico](https://raw.githubusercontent.com/chatra-lang/resources/master/images/calico.jpg "Mike \(calico\) is not simple")


## Chatra is in alpha stage

Chatra development is in early phase.   
There are still many features that are lacking in the language, 
and we plan to add them in the future.
At least the following important features are missing or poorly implemented:

- Standard (embedded) library
- Debugger support
- Remote procedure call support
- Faster execution
- Annotation and reflection support
- Multi-language support
- Hot-fix support
- Accurate and detailed documents
- Various tests covering entire features

Until the version number reaches to 1.0,
there is a possibility that changes will be made that breaks the compatibility
with older versions
(We think it is important that quickly implementing new features,
rather than to keep the compatibility between the versions). 
When incorporating Chatra into your project, 
please pay attention to differences between Chatra’s versions.


## Contributing

Chatra is an open source project 
and humbly accepts any contributions you might make to the project. 
Any contribution even if you think that is small, makes a great difference.  
Not only writing code, 
but also we look forward to any kinds of participation such as:

- Bug report
- Adding test patterns
- Request of features
- Writing documents, making translation or correction
- Constructing and/or managing a website
- Graphical design
- Sending impressions or feelings

If there are, please post them by email, PRs, or any communication tools you can reach to us.


## License

Released under Apache License v2.


## Version History

0.2.0  Bug fix & adding some embedded libraries

0.1.0  First release
