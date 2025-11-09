import type { Module, RecordKey, RecordLocation, ResolvedType } from "soiac";

/**
 * Transforms a type found in a `.soia` file into a C++ type.
 */
export class TypeSpeller {
  constructor(
    readonly recordMap: ReadonlyMap<RecordKey, RecordLocation>,
    private readonly origin: Module,
    private readonly includes: Set<string>,
  ) {}

  getCcType(
    type: ResolvedType,
    opts: {
      fieldIsRecursive?: boolean;
      forceNamespace?: boolean;
    } = {},
  ): string {
    switch (type.kind) {
      case "record": {
        const record = this.recordMap.get(type.key)!;
        let qualifiedName = getClassName(record);
        if (record.modulePath !== this.origin.path || opts.forceNamespace) {
          // The record is located in an imported module.
          const path = record.modulePath;
          if (record.modulePath !== this.origin.path) {
            this.includes.add('"soiagen/' + path.replace(/\.soia$/, '.h"'));
          }
          const namespace = modulePathToNamespace(path);
          qualifiedName = `::${namespace}::${qualifiedName}`;
        }
        if (opts.fieldIsRecursive) {
          return `::soia::rec<${qualifiedName}>`;
        }
        return qualifiedName;
      }
      case "array": {
        const itemType = this.getCcType(type.item, opts);
        const { key } = type;
        if (key) {
          const { path } = key;
          let keyType = "";
          for (const pathItem of path) {
            const fieldName = pathItem.name.text;
            const isLastField = pathItem === path.at(-1);
            if (isLastField && key.keyType.kind === "record") {
              keyType = `::soia::get_kind<${keyType}>`;
            } else {
              keyType = `::soiagen::get_${fieldName}<${keyType}>`;
            }
          }
          return `::soia::keyed_items<${itemType}, ${keyType}>`;
        } else {
          return `::std::vector<${itemType}>`;
        }
      }
      case "optional": {
        const valueType = this.getCcType(type.other, opts);
        return `::absl::optional<${valueType}>`;
      }
      case "primitive": {
        const { primitive } = type;
        switch (primitive) {
          case "bool":
            return "bool";
          case "int32":
            return "::int32_t";
          case "int64":
            return "::int64_t";
          case "uint64":
            return "::uint64_t";
          case "float32":
            return "float";
          case "float64":
            return "double";
          case "timestamp":
            return "::absl::Time";
          case "string":
            return "::std::string";
          case "bytes":
            return "::soia::ByteString";
        }
      }
    }
  }
}

export function modulePathToNamespace(path: string): string {
  return "soiagen_" + path.replace(/\.soia$/, "").replace("/", "_");
}

export function getClassName(record: RecordLocation): string {
  return record.recordAncestors.map((r) => r.name.text).join("_");
}
