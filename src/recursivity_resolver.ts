import { Field, Module, RecordKey, RecordLocation, ResolvedType } from "soiac";

/**
 * In C++, a struct type can not refer to itself directly. The only way to create a
 * recursive struct is to use pointers or containers which allocate their elements on
 * the heap, e.g. `std::vector`. Neither `absl::optional` nor `absl::variant` qualify
 * because they allocate the element on the stack.
 *
 * When generating C++ types from recursive Soia types, Soia uses `rec<T>` instead of
 * `T` for the fields which cause the type to be recursive.
 *
 * Consider this example:
 *
 *   struct Foo {
 *     bar: Bar;
 *   }
 *
 *   struct Bar {
 *     foo: Foo?;
 *     x: int32;
 *   }
 *
 * Both Foo and Bar are recursive since Foo refers to Bar and Bar refers to Foo, so Foo
 * indirectly refers to itself and so does Bar.
 *
 * This is the C++ code Soia generates:
 *   struct Foo;
 *   struct Bar;
 *
 *   struct Foo {
 *     rec<Bar> bar;
 *   };
 *
 *   struct Bar {
 *     absl::optional<Foo> foo;
 *     int32 x = 0;
 *   };
 *
 * Because the type of Foo:bar is `rec<Bar>` instead of `Bar`, Foo no longer refers to
 * Bar and thus Foo no longer refers to itself, and Bar no longer refers to itself
 * either: problem solved.
 * The same outcome would have been reached by changing the type of Bar::foo instead of
 * Foo::bar.
 *
 * This class finds the fields of the soia types which should have a `rec<...>` type in
 * their C++ representation.
 */
export class RecursvityResolver {
  static resolve(
    recordMap: ReadonlyMap<RecordKey, RecordLocation>,
    module: Module,
  ): RecursvityResolver {
    const sortedRecords = [...module.records].sort((a, b) => {
      const aKey = a.recordAncestors.map((r) => r.name).join(".");
      const bKey = b.recordAncestors.map((r) => r.name).join(".");
      return aKey.localeCompare(bKey, "en-US");
    });
    const recursiveFields = new Set<Field>();
    const recordToDeps = new Map<RecordKey, Set<RecordKey>>();
    for (const record of sortedRecords) {
      const recordDeps = new Set<RecordKey>();
      for (const field of record.record.fields) {
        if (!field.type) continue;
        const depsCollector = new DepsCollector(
          recordMap,
          module.path,
          recursiveFields,
        );
        depsCollector.collect(field.type);
        if (depsCollector.deps.has(record.record.key)) {
          recursiveFields.add(field);
        } else {
          depsCollector.deps.forEach((dep) => recordDeps.add(dep));
        }
      }
      recordToDeps.set(record.record.key, recordDeps);
    }
    const reorderedRecords = reorderRecords(
      sortedRecords,
      recordToDeps,
      recordMap,
    );
    return new RecursvityResolver(recursiveFields, reorderedRecords);
  }

  private constructor(
    private readonly recursiveFields: ReadonlySet<Field>,
    /**
     * All records declared in the module, reordered sp every record appears after its
     * dependencies.
     */
    readonly reorderedRecords: readonly RecordLocation[],
  ) {}

  isRecursive(field: Field): boolean {
    return this.recursiveFields.has(field);
  }
}

class DepsCollector {
  constructor(
    readonly recordMap: ReadonlyMap<RecordKey, RecordLocation>,
    readonly modulePath: string,
    readonly recursiveFields: ReadonlySet<Field>,
  ) {}

  readonly deps = new Set<RecordKey>();

  collect(type: ResolvedType): void {
    switch (type.kind) {
      case "optional":
        // absl::optional allocates its value on the stack.
        this.collect(type.other);
        break;
      case "record": {
        const recordKey = type.key;
        const record = this.recordMap.get(recordKey)!;
        // We only care about records declared in the module.
        if (record.modulePath !== this.modulePath) break;
        if (this.deps.has(recordKey)) break;
        this.deps.add(recordKey);
        // Value fields of enums are allocated on the heap.
        if (record.record.recordType === "enum") break;
        for (const field of record.record.fields) {
          if (!field.type) continue;
          if (this.recursiveFields.has(field)) continue;
          this.collect(field.type);
        }
      }
    }
  }
}

function reorderRecords(
  inputRecords: readonly RecordLocation[],
  recordToDeps: ReadonlyMap<RecordKey, Set<RecordKey>>,
  recordMap: ReadonlyMap<RecordKey, RecordLocation>,
): readonly RecordLocation[] {
  const result: RecordLocation[] = [];
  const seenRecords = new Set<RecordKey>();
  function addRecord(record: RecordLocation): void {
    const { key } = record.record;
    if (seenRecords.has(key)) return;
    seenRecords.add(key);
    const deps = recordToDeps.get(key)!;
    for (const dep of deps) {
      addRecord(recordMap.get(dep)!);
    }
    result.push(record);
  }
  for (const record of inputRecords) {
    addRecord(record);
  }
  return result;
}
