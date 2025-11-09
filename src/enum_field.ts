import { Field, ResolvedType, convertCase } from "soiac";
import { TypeSpeller } from "./type_speller.js";

interface MutableEnumField {
  /** As specified in the .soia file. */
  readonly fieldName: string;
  /** Examples: "bool", "Foo". Empty if the field is a constant field. */
  readonly valueType: string;
  /** Examples: "bool", "foo::Foo". Empty if the field is a constant field. */
  readonly valueTypeWithNamespace: string;
  /** As specified in the .soia file. */
  fieldNumber: number;
  /** Whether the field is the special UNKNOWN field. */
  isUnknownField: boolean;
  /** Examples: "f_field", "wrap_field". */
  readonly structType: string;
  /** Example: "wrap_field_type". Empty if the field is a constant field. */
  readonly typeAlias: string;
  /** kField or wrap_field. */
  readonly identifier: string;
  /** kConstField or kValField */
  readonly kindEnumerator: string;
  /**
   * True if the field is a value field and the value should be allocated on the
   * heap.
   */
  readonly usePointer: boolean;
}

export type EnumField = Readonly<MutableEnumField>;

export function getEnumFields(
  fields: readonly Field[],
  typeSpeller: TypeSpeller,
): readonly EnumField[] {
  const result: MutableEnumField[] = [];
  result.push(makeUnknownField());
  for (const inField of fields) {
    let outField: MutableEnumField;
    if (inField.type) {
      outField = makeValueField(inField, typeSpeller);
    } else {
      outField = makeConstantField(inField.name.text);
    }
    outField.fieldNumber = inField.number;
    result.push(outField);
  }
  return result;
}

function makeUnknownField(): MutableEnumField {
  return {
    fieldName: "?",
    valueType: "",
    valueTypeWithNamespace: "",
    fieldNumber: 0,
    isUnknownField: true,
    structType: `k_unknown`,
    typeAlias: "",
    identifier: `kUnknown`,
    kindEnumerator: `kConstUnknown`,
    usePointer: false,
  };
}

function makeConstantField(fieldName: string): MutableEnumField {
  const lowerUnderscore = convertCase(
    fieldName,
    "UPPER_UNDERSCORE",
    "lower_underscore",
  );
  const upperCamel = convertCase(fieldName, "UPPER_UNDERSCORE", "UpperCamel");
  return {
    fieldName: fieldName,
    valueType: "",
    valueTypeWithNamespace: "",
    fieldNumber: 0,
    isUnknownField: false,
    structType: `k_${lowerUnderscore}`,
    typeAlias: "",
    identifier: `k${upperCamel}`,
    kindEnumerator: `kConst${upperCamel}`,
    usePointer: false,
  };
}

function makeValueField(
  field: Field,
  typeSpeller: TypeSpeller,
): MutableEnumField {
  const fieldName = field.name.text;
  const upperCamel = convertCase(fieldName, "lower_underscore", "UpperCamel");
  const type = field.type!;
  return {
    fieldName: fieldName,
    valueType: typeSpeller.getCcType(type),
    valueTypeWithNamespace: typeSpeller.getCcType(type, {
      forceNamespace: true,
    }),
    fieldNumber: 0,
    isUnknownField: false,
    structType: `wrap_${fieldName}`,
    typeAlias: `wrap_${fieldName}_type`,
    identifier: `wrap_${fieldName}`,
    kindEnumerator: `kVal${upperCamel}`,
    usePointer: usePointer(field.type!),
  };
}

function usePointer(type: ResolvedType): boolean {
  if (type.kind !== "primitive") return true;
  switch (type.primitive) {
    case "bool":
    case "int32":
    case "int64":
    case "uint64":
    case "float32":
    case "float64":
    case "timestamp":
      return false;
  }
  return true;
}
