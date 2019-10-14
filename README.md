![chatra_logo](https://raw.githubusercontent.com/chatra-lang/resources/master/logo/chatra_logo_for_light_bg_small.png "Chatra")

Japanese version is [here](/README.ja.md).

# About “Chatra”
Chatra is a lightweight scripting language which has simple design 
so that anyone can write its scripts easily.   
It can be used as an independent interpreter, or it can be embedded in your program
and used as a macro language.

To make it easy to embed into the programs, 
the Chatra interpreter is written using only C++11 standard. 
It does not depend on any external libraries.  
You can run multiple independent virtual machines without any special tricks, 
and you can run multiple scripts inside each virtual machine at the same time.

## Chatra is simple
Chatra has very simple grammar.  
If you are familiar with at least one programming language, you can read it.

```Text
class HelloWorld
    def someMethod(to: 'Earth')
        log('Hello,' + to + '!')

instance = HelloWorld()
for in Range(5)
    instance.someMethod('World')
```

Moreover, Chatra has many features that modern languages should have.

Intentionally saying it complicated, 
Chatra is a class-style dynamic typing object-oriented language with features such as
multi-threading support, concurrent garbage collection, closure, 
and reference-based mutual exclusion. 
But simplicity is the core value of Chatra, 
so you can forget about such a tedious story. 
Remember only if you want to explain it to someone cool :-)

Chatra's grammar is influenced by languages such as
C/C++, Java, Python and Swift.
If you have learned these languages, you can use Chatra immediately.

## Chatra is serializable
The second key feature is that Chatra is serializable.   
A virtual machine (called Runtime) that executes Chatra scripts 
can always save the entire state into a byte stream, 
and it can be restored and resumed later. 
This is useful in situations where frequent stop/resume occurs, 
such as smartphone apps.  
If the conditions are met, it can even be resumed on another machine[^1].

[^1]: Strictly saying, there are some prerequisites.

note: The following code is useful information for those who want to embed Chatra’s virtual machine into a program, but is probably unusable information for those who write scripts of Chatra.

```C++
// Generate Runtime and execute "test_script".
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

The name of “Chatra” comes from the Japanese “red tabby”. 
Red tabbies are simple and friendly, 
so their characteristics is suitable for this language.

![red_tabby](https://raw.githubusercontent.com/chatra-lang/resources/master/images/red_tabby.jpg "Chatra \(red tabby\) is simple")
![calico](https://raw.githubusercontent.com/chatra-lang/resources/master/images/calico.jpg "Mike \(calico\) is not simple")

# Getting Started
(Sorry under construction)  
There is a document that outlines the language specification.  
Details are here. However, it is not fully maintained yet.  
If you want to know how to incorporate Chatra’s virtual machine
into your program, go here.

At present, Chatra is provided in the form of source code written in C++.
To build them, please check this document.
The source code also includes a front-end
for using as an independent interpreter. 
See here for how to use the front-end.

# Chatra is in alpha stage
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

# Contributing
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

If there are, please post them by ~~this procedure~~.  
(Sorry documentation is now under construction; We accept PRs via github)

# License
Released under Apache License v2.

# Version History
0.1.0  First release
