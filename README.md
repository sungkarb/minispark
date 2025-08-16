# MiniSpark

## Overview
MiniSpark is a mini implementation of distributed data processing frameworks like Spark and MapReduce, created as a project for CS 537 (Introduction to Operating Systems). It processes data through a directed acyclic graph (DAG) of transformations, using multi-threading to achieve parallelism on a single node.

Distributed data processing frameworks are crucial for performing data analytics on large quantities of data. Frameworks like MapReduce and Spark are powerful and relatively simple to program. Users write declarative queries to manipulate data, and the framework processes the data, automatically handling difficult problems of distributed computing -- parallel processing, inter-process communication, and fault tolerance.

## Learning Objectives
To learn about data processing pipelines
To implement a correct MiniSpark framework with several common data processing operators
To efficiently process data in parallel using threads

## Project Structure
├── applications/      # Example applications using MiniSpark
├── lib/               # Core library code
├── solution/          # Implementation of MiniSpark
├── sample-files/      # Test input files
├── tests/             # Test suite
└── Makefile           # Build system

## Background
To understand how to make progress on any project that involves concurrency, you should understand the basics of thread creation, mutual exclusion (with locks), and signaling/waiting (with condition variables).

###  Key Concepts
RDDs (Resilient Distributed Datasets)
Data is represented as immutable RDDs that form the nodes in a DAG where edges represent transformations.

### Transformations
map: Apply a function to each element
filter: Keep elements that satisfy a predicate
join: Combine elements from two RDDs with matching keys
partitionBy: Redistribute data across partitions
Actions
count: Return the number of elements in an RDD
print: Display each element in an RDD

### Actions
count: Return the number of elements in an RDD
print: Display each element in an RDD

## Building the Project
To build MiniSpark and its example applications:
```bash
make
```

## Running Tests
```bash
cd tests
./run-tests.sh
```

## Parallelism
MiniSpark achieves parallelism through:

Materializing partitions of an RDD in parallel
Computing independent parts of the DAG concurrently

## About
This project was completed as part of CS 537 (Introduction to Operating Systems) to demonstrate understanding of concurrent programming and distributed data processing concepts.
