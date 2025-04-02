import { EnumField, getEnumFields } from "./enum_field.js";
import { CC_KEYWORDS } from "./keywords.js";
import { RecursvityResolver } from "./recursivity_resolver.js";
import {
  TypeSpeller,
  getClassName,
  modulePathToNamespace,
} from "./type_speller.js";
import {
  type CodeGenerator,
  type Constant,
  Field,
  type Method,
  type Module,
  type RecordKey,
  type RecordLocation,
  convertCase,
  encodeInt32,
  simpleHash,
} from "soiac";
import { z } from "zod";

const Config = z.object({
  writeGoogleTestHeaders: z.boolean(),
});

type Config = z.infer<typeof Config>;

class CcCodeGenerator implements CodeGenerator<Config> {
  readonly id = "cc";
  readonly configType = Config;
  readonly version = "1.0.0";

  generateCode(input: CodeGenerator.Input<Config>): CodeGenerator.Output {
    const { recordMap, config } = input;
    const outputFiles: CodeGenerator.OutputFile[] = [];

    for (const module of input.modules) {
      const generator = new CcLibFilesGenerator(module, recordMap, config);
      outputFiles.push({
        path: module.path.replace(/\.soia$/, ".h"),
        code: generator.getCode(".h"),
      });
      outputFiles.push({
        path: module.path.replace(/\.soia$/, ".cc"),
        code: generator.getCode(".cc"),
      });
      if (config.writeGoogleTestHeaders) {
        outputFiles.push({
          path: module.path.replace(/\.soia$/, ".testing.h"),
          code: generator.getCode(".testing.h"),
        });
      }
    }

    return { files: outputFiles };
  }
}

/**
 * Generates the code for one C++ library, made of one .h file and one .cc
 * file.
 */
class CcLibFilesGenerator {
  constructor(
    private readonly inModule: Module,
    private readonly recordMap: ReadonlyMap<RecordKey, RecordLocation>,
    config: Config,
  ) {
    this.typeSpeller = new TypeSpeller(recordMap, inModule, this.includes);
    this.recursivityResolver = RecursvityResolver.resolve(recordMap, inModule);
    this.includes.add('"soia.h"');
    this.namespace = modulePathToNamespace(inModule.path);
    this.generate();
  }

  getCode(extension: ".h" | ".cc" | ".testing.h"): string {
    switch (extension) {
      case ".h":
        return fileContentsToCode(this.header);
      case ".cc":
        return fileContentsToCode(this.source);
      case ".testing.h":
        return fileContentsToCode(this.testingHeader);
    }
  }

  private generate(): void {
    this.header.namespace = this.namespace;
    this.source.namespace = this.namespace;
    this.testingHeader.namespace = this.namespace;
    for (const record of this.recursivityResolver.reorderedRecords) {
      this.writeCodeForRecord(record);
    }
    for (const method of this.inModule.methods) {
      this.writeCodeForMethod(method);
    }
    for (const constant of this.inModule.constants) {
      this.writeCodeForConstant(constant);
    }
    this.writeIncludes();
  }

  private writeCodeForRecord(record: RecordLocation): void {
    const { recordType } = record.record;
    if (recordType === "struct") {
      this.writeCodeForStruct(record);
    } else {
      this.writeCodeForEnum(record);
    }
    this.writeCodeInHeaderForAdapter(record);
  }

  private writeCodeForStruct(struct: RecordLocation): void {
    const { header, recordMap, source, testingHeader, typeSpeller } = this;

    const { fields, numSlots, numSlotsInclRemovedNumbers, nestedRecords } =
      struct.record;
    const fieldsByName = [...struct.record.fields].sort((a, b) =>
      a.name.text.localeCompare(b.name.text, "en-US"),
    );
    const fieldsByNumber = [...struct.record.fields].sort(
      (a, b) => a.number - b.number,
    );

    const className = getClassName(struct);
    const adapterName = `${className}Adapter`;
    const qualifiedName = `::${this.namespace}::${className}`;
    const constRefType = `const ${qualifiedName}&`;

    // ------------------------------
    // - APPEND CODE IN THE .h FILE -
    // ------------------------------

    header.mainTop.push(`struct ${className};`);

    for (const field of fields) {
      const fieldName = field.name.text;
      const escapedFieldName = maybeEscapeLowerCaseName(fieldName);
      const structName = `get_${fieldName}`;
      if (!this.addSoiagenSymbol(structName)) continue;
      header.soiagen.push(`#ifndef SOIAGEN_${structName}`);
      header.soiagen.push(`#define SOIAGEN_${structName}`);
      header.soiagen.push("template <typename other = ::soia::identity>");
      header.soiagen.push(`struct ${structName} {`);
      header.soiagen.push("  using other_type = other;");
      header.soiagen.push("");
      header.soiagen.push(
        `  static constexpr absl::string_view kFieldName = "${fieldName}";`,
      );
      header.soiagen.push("");
      header.soiagen.push("  template <typename T>");
      header.soiagen.push(`  auto& operator()(T& input) const {`);
      header.soiagen.push(
        `    return soia_internal::get(other()(input).${escapedFieldName});`,
      );
      header.soiagen.push("  }");
      header.soiagen.push("};");
      header.soiagen.push("#endif");
      header.soiagen.push("");
    }

    header.mainMiddle.push(`struct ${className} {`);
    // Declare fields in alphabetical order. It helps users who want to
    // initialize a struct using the designated initializer syntax. See:
    // https://abseil.io/tips/172
    for (const field of fieldsByName) {
      const type = field.type!;
      const fieldIsRecursive = this.recursivityResolver.isRecursive(field);
      const ccType = typeSpeller.getCcType(type, {
        fieldIsRecursive: fieldIsRecursive,
      });
      const fieldName = maybeEscapeLowerCaseName(field.name.text);
      // Numeric types must be initialized.
      let assignment = "";
      if (type.kind === "primitive") {
        if (type.primitive === "bool") {
          assignment = " = false";
        } else if (type.primitive.includes("int")) {
          assignment = " = 0";
        } else if (type.primitive.includes("float")) {
          assignment = " = 0.0";
        }
      }
      header.mainMiddle.push(`  ${ccType} ${fieldName}${assignment};`);
    }
    header.mainMiddle.push("");
    header.mainMiddle.push("  ::soia_internal::UnrecognizedFields<");
    header.mainMiddle.push(
      `      ::soia_internal::${this.namespace}::${adapterName}>`,
    );
    header.mainMiddle.push("      _unrecognized;");
    header.mainMiddle.push("");
    header.mainMiddle.push(
      `  bool operator==(const ${className}& other) const;`,
    );
    header.mainMiddle.push("");
    header.mainMiddle.push(
      `  inline bool operator!=(const ${className}& other) const {`,
    );
    header.mainMiddle.push("    return !(*this == other);");
    header.mainMiddle.push("  }");
    header.mainMiddle.push("");
    header.mainMiddle.push("  struct whole {");
    for (const field of fieldsByName) {
      const type = field.type!;
      const fieldIsRecursive = this.recursivityResolver.isRecursive(field);
      const ccType = typeSpeller.getCcType(type, {
        fieldIsRecursive: fieldIsRecursive,
      });
      const fieldName = maybeEscapeLowerCaseName(field.name.text);
      header.mainMiddle.push(`    ::soia::must_init<${ccType}> ${fieldName};`);
    }
    header.mainMiddle.push("");
    header.mainMiddle.push(`    operator ${className}();`);
    header.mainMiddle.push("  };");
    header.mainMiddle.push("");
    for (const nestedRecord of nestedRecords) {
      let typeAlias = nestedRecord.name.text;
      if (typeAlias === className) {
        typeAlias = `${typeAlias}_`;
      }
      const recordLocation = recordMap.get(nestedRecord.key)!;
      const nestedClassName = getClassName(recordLocation);
      header.mainMiddle.push(`  using ${typeAlias} = ${nestedClassName};`);
    }
    header.mainMiddle.push("};");
    header.mainMiddle.push("");
    header.mainBottom.push("inline std::ostream& operator<<(");
    header.mainBottom.push("    std::ostream& os,");
    header.mainBottom.push(`    ${constRefType} input) {`);
    header.mainBottom.push(
      "  return os << ::soia_internal::ToDebugString(input);",
    );
    header.mainBottom.push("}");
    header.mainBottom.push("");
    {
      header.mainBottom.push("template <typename H>");
      header.mainBottom.push(`H AbslHashValue(H h, ${constRefType} input) {`);
      const args = fields
        .map((f) => `,\n      input.${maybeEscapeLowerCaseName(f.name.text)}`)
        .join("");
      header.mainBottom.push("  return H::combine(");
      header.mainBottom.push(`      std::move(h)${args});`);
      header.mainBottom.push("}");
      header.mainBottom.push("");
    }

    // --------------------------------------
    // - APPEND CODE IN THE .testing.h FILE -
    // --------------------------------------

    testingHeader.soiagen.push("template <>");
    testingHeader.soiagen.push(`struct StructIs<${qualifiedName}> {`);
    // Declare fields in alphabetical order. It helps users who want to
    // initialize a struct using the designated initializer syntax. See:
    // https://abseil.io/tips/172
    for (const field of fieldsByName) {
      const type = field.type!;
      const fieldIsRecursive = this.recursivityResolver.isRecursive(field);
      const fieldName = maybeEscapeLowerCaseName(field.name.text);
      if (type.kind === "record") {
        // Do not pass fieldIsRecursive.
        const ccType = typeSpeller.getCcType(type, { forceNamespace: true });
        const recordType = recordMap.get(type.key)!.record.recordType;
        if (!fieldIsRecursive && recordType === "struct") {
          testingHeader.soiagen.push(`  StructIs<${ccType}> ${fieldName};`);
        } else {
          testingHeader.soiagen.push(`  Matcher<${ccType}> ${fieldName} = _;`);
        }
      } else {
        const ccType = typeSpeller.getCcType(type, {
          fieldIsRecursive: fieldIsRecursive,
          forceNamespace: true,
        });
        testingHeader.soiagen.push(`  Matcher<${ccType}> ${fieldName} = _;`);
      }
    }
    testingHeader.soiagen.push("");
    testingHeader.soiagen.push(
      `  Matcher<${qualifiedName}> ToMatcher() const {`,
    );
    if (fields.length <= 0) {
      testingHeader.soiagen.push("    return _;");
    } else {
      testingHeader.soiagen.push(
        `    return ::testing::soia_internal::StructIs<${qualifiedName}>(`,
      );
      for (const field of fieldsByName) {
        const type = field.type!;
        const fieldIsRecursive = this.recursivityResolver.isRecursive(field);
        const fieldName = field.name.text;
        let matcherExpr = maybeEscapeLowerCaseName(fieldName);
        if (type.kind === "record") {
          const recordType = recordMap.get(type.key)!.record.recordType;
          if (!fieldIsRecursive && recordType === "struct") {
            matcherExpr += ".ToMatcher()";
          }
        }
        const end = field === fieldsByName.at(-1) ? ");" : ",";
        const getterExpr = `::soiagen::get_${fieldName}()`;
        testingHeader.soiagen.push(
          `        std::make_pair(${getterExpr}, ${matcherExpr})${end}`,
        );
      }
    }
    testingHeader.soiagen.push("  }");
    testingHeader.soiagen.push("");
    testingHeader.soiagen.push("  template <typename T>");
    testingHeader.soiagen.push("  operator Matcher<T>() const {");
    testingHeader.soiagen.push(
      "    return ::testing::SafeMatcherCast<T>(ToMatcher());",
    );
    testingHeader.soiagen.push("  }");
    testingHeader.soiagen.push("");
    testingHeader.soiagen.push("};");
    testingHeader.soiagen.push("");

    // -------------------------------
    // - APPEND CODE IN THE .cc FILE -
    // -------------------------------

    {
      // _GetArrayLength(const T&)
      source.anonymous.push("inline ::int32_t _GetArrayLength(");
      source.anonymous.push(`    ${constRefType} input,`);
      source.anonymous.push(
        "    const std::shared_ptr<soia_internal::UnrecognizedFieldsData>& u) {",
      );
      source.anonymous.push("  if (u != nullptr)");
      source.anonymous.push("    return u->array_len;");
      for (const field of [...fieldsByNumber].reverse()) {
        const { number, name } = field;
        const fieldExpr = `input.${maybeEscapeLowerCaseName(name.text)}`;
        source.anonymous.push(
          `  if (!::soia_internal::IsDefault(${fieldExpr}))`,
        );
        source.anonymous.push(`    return ${number + 1};`);
      }
      source.anonymous.push("  return 0;");
      source.anonymous.push("}");
      source.anonymous.push("");
    }

    {
      // IsDefault(const T&)
      source.internalMain.push(
        `bool ${adapterName}::IsDefault(const type& input) {`,
      );
      const expression = fields.length
        ? fields
            .map((f) => {
              const fieldName = maybeEscapeLowerCaseName(f.name.text);
              return `::soia_internal::IsDefault(input.${fieldName})`;
            })
            .join("\n      && ")
        : "true";
      source.internalMain.push(`  return ${expression};`);
      source.internalMain.push("}");
      source.internalMain.push("");
    }

    {
      // Append(const T&, DenseJson&)
      source.internalMain.push(
        `void ${adapterName}::Append(const type& input, DenseJson& out) {`,
      );
      source.internalMain.push(
        "  const auto& unrecognized = input._unrecognized.data;",
      );
      source.internalMain.push(
        "  const auto array_len = _GetArrayLength(input, unrecognized);",
      );
      source.internalMain.push("  if (array_len == 0) {");
      source.internalMain.push("    out.out += {'[', ']'};");
      source.internalMain.push("    return;");
      source.internalMain.push("  }");
      source.internalMain.push("  JsonArrayCloser closer(&out);");
      let charLiterals = ["'['"];
      let lastFieldNumber = -1;
      for (const field of fieldsByNumber) {
        const { number, name } = field;
        const isFirstField = field === fields[0];
        // Append one 0 for every removed number.
        for (let i = lastFieldNumber + 1; i < number; ++i) {
          charLiterals.push("'0'");
          charLiterals.push("','");
        }
        source.internalMain.push(`  out.out += {${charLiterals.join(", ")}};`);
        charLiterals = [];
        const fieldExpr = `input.${maybeEscapeLowerCaseName(name.text)}`;
        source.internalMain.push(
          `  ::soia_internal::Append(${fieldExpr}, out);`,
        );
        source.internalMain.push(`  if (array_len == ${number + 1}) return;`);
        lastFieldNumber = number;
        charLiterals.push("','");
      }
      for (let i = numSlots; i < numSlotsInclRemovedNumbers; ++i) {
        charLiterals.push("'0'");
        charLiterals.push("','");
      }
      source.internalMain.push(`  out.out += {${charLiterals.join(", ")}};`);
      source.internalMain.push("  unrecognized->values.AppendTo(out);");
      source.internalMain.push("}");
      source.internalMain.push("");
    }

    {
      // Append(const T&, ReadableJson&)
      source.internalMain.push(
        `void ${adapterName}::Append(const type& input, ReadableJson& out) {`,
      );
      if (fields.length) {
        source.internalMain.push("  JsonObjectWriter(&out)");
        for (const field of fields) {
          const isLastField = field === fields.at(-1);
          const maybeSemicolon = isLastField ? ";" : "";
          const name = field.name.text;
          const fieldExpr = `input.${maybeEscapeLowerCaseName(name)}`;
          source.internalMain.push(
            `      .Write("${name}", ${fieldExpr})${maybeSemicolon}`,
          );
        }
      } else {
        source.internalMain.push("  out.out += {'{', '}'};");
      }
      source.internalMain.push("}");
      source.internalMain.push("");
    }

    {
      // Append(const T&, DebugString&)
      source.internalMain.push(
        `void ${adapterName}::Append(const type& input, DebugString& out) {`,
      );
      if (fields.length) {
        source.internalMain.push("  DebugObjectWriter(&out)");
        for (const field of fields) {
          const isLastField = field === fields.at(-1);
          const maybeSemicolon = isLastField ? ";" : "";
          const name = maybeEscapeLowerCaseName(field.name.text);
          const fieldExpr = `input.${name}`;
          source.internalMain.push(
            `      .Write("${name}", ${fieldExpr})${maybeSemicolon}`,
          );
        }
      } else {
        source.internalMain.push("  out.out += {'{', '}'};");
      }
      source.internalMain.push("}");
      source.internalMain.push("");
    }

    {
      // Append(const T&, ByteSink&)
      source.internalMain.push(
        `void ${adapterName}::Append(const type& input, ByteSink& out) {`,
      );
      source.internalMain.push(
        "  const auto& unrecognized = input._unrecognized.data;",
      );
      source.internalMain.push(
        "  const auto array_len = _GetArrayLength(input, unrecognized);",
      );
      source.internalMain.push("  if (array_len == 0) {");
      source.internalMain.push("    out.Push(246);");
      source.internalMain.push("    return;");
      source.internalMain.push("  }");
      source.internalMain.push("  AppendArrayPrefix(array_len, out);");
      let lastFieldNumber = -1;
      for (const field of fieldsByNumber) {
        const { number, name } = field;
        const isLastField = field === fieldsByNumber.at(-1);
        if (lastFieldNumber < number - 1) {
          // Append one 0 for every removed number.
          const zeros = "0, ".repeat(number - lastFieldNumber - 1).slice(0, -2);
          source.internalMain.push(`  out.Push(${zeros});`);
        }
        const fieldExpr = `input.${maybeEscapeLowerCaseName(name.text)}`;
        source.internalMain.push(
          `  ::soia_internal::Append(${fieldExpr}, out);`,
        );
        if (!isLastField) {
          source.internalMain.push(`  if (array_len == ${number + 1}) return;`);
        }
        lastFieldNumber = number;
      }
      source.internalMain.push(
        `  if (array_len == ${lastFieldNumber + 1}) return;`,
      );
      if (numSlots < numSlotsInclRemovedNumbers) {
        // Append one 0 for every removed number at the end of the struct.
        const zeros = "0, "
          .repeat(numSlotsInclRemovedNumbers - numSlots)
          .slice(0, -2);
        source.internalMain.push(`  out.Push(${zeros});`);
      }
      source.internalMain.push("  unrecognized->values.AppendTo(out);");
      source.internalMain.push("}");
      source.internalMain.push("");
    }

    {
      // Parse(JsonTokenizer&, T&)
      source.internalMain.push(`void ${adapterName}::Parse(`);
      source.internalMain.push("    JsonTokenizer& tokenizer,");
      source.internalMain.push("    type& out) {");
      source.internalMain.push("  switch (tokenizer.state().token_type) {");
      source.internalMain.push("    case JsonTokenType::kLeftSquareBracket: {");
      source.internalMain.push(
        "      JsonArrayReader array_reader(&tokenizer);",
      );
      let lastNumber = -1;
      for (const field of fieldsByNumber) {
        const ccFieldName = maybeEscapeLowerCaseName(field.name.text);
        source.internalMain.push(
          "      if (!array_reader.NextElement()) break;",
        );
        for (let i = lastNumber + 1; i < field.number; ++i) {
          source.internalMain.push("      SkipValue(tokenizer);");
          source.internalMain.push(
            "      if (!array_reader.NextElement()) break;",
          );
        }
        source.internalMain.push(
          `      ::soia_internal::Parse(tokenizer, out.${ccFieldName});`,
        );
        lastNumber = field.number;
      }
      source.internalMain.push("      if (!array_reader.NextElement()) break;");
      source.internalMain.push(
        "      auto& unrecognized = out._unrecognized.data;",
      );
      const args = [
        "array_reader",
        numSlots,
        numSlotsInclRemovedNumbers,
        "unrecognized",
      ].join(", ");
      source.internalMain.push(
        `      ::soia_internal::ParseUnrecognizedFields(${args});`,
      );
      source.internalMain.push("      break;");
      source.internalMain.push("    }");
      source.internalMain.push("    case JsonTokenType::kLeftCurlyBracket: {");
      const parserExpr =
        "(new StructJsonObjectParser<type>())" +
        fields
          .map((field) => {
            const name = field.name.text;
            const ccFieldName = maybeEscapeLowerCaseName(name);
            const indent = "              ";
            return `\n${indent}->AddField("${name}", &type::${ccFieldName})`;
          })
          .join("");
      source.internalMain.push("      static const auto* kParser =");
      source.internalMain.push(`          ${parserExpr};`);
      source.internalMain.push("      kParser->Parse(tokenizer, out);");
      source.internalMain.push("      break;");
      source.internalMain.push("    }");
      source.internalMain.push("    case JsonTokenType::kZero:");
      source.internalMain.push("      tokenizer.Next();");
      source.internalMain.push("      break;");
      source.internalMain.push("    default: {");
      source.internalMain.push(
        "      tokenizer.mutable_state().PushUnexpectedTokenError(\"'['\");",
      );
      source.internalMain.push("    }");
      source.internalMain.push("  }");
      source.internalMain.push("}");
      source.internalMain.push("");
    }

    {
      // Parse(ByteSource&, T&)
      source.internalMain.push(
        `void ${adapterName}::Parse(ByteSource& source, type& out) {`,
      );
      source.internalMain.push("  ::uint32_t array_len = 0;");
      source.internalMain.push("  ParseArrayPrefix(source, array_len);");
      let lastNumber = -1;
      for (const field of fieldsByNumber) {
        const ccFieldName = maybeEscapeLowerCaseName(field.name.text);
        for (let i = lastNumber + 1; i < field.number; ++i) {
          source.internalMain.push(`  if (array_len == ${i}) return;`);
          source.internalMain.push("  SkipValue(source);");
        }
        source.internalMain.push(`  if (array_len == ${field.number}) return;`);
        source.internalMain.push(
          `  ::soia_internal::Parse(source, out.${ccFieldName});`,
        );
        lastNumber = field.number;
      }
      source.internalMain.push(`  if (array_len == ${numSlots}) return;`);
      source.internalMain.push(
        "  auto& unrecognized = out._unrecognized.data;",
      );
      const args = [
        "source",
        "array_len",
        numSlots,
        numSlotsInclRemovedNumbers,
        "unrecognized",
      ].join(", ");
      source.internalMain.push(
        `  ::soia_internal::ParseUnrecognizedFields(${args});`,
      );
      source.internalMain.push("}");
      source.internalMain.push("");
    }

    {
      // GetType(soia_type<T>)
      source.internalMain.push(
        `soia::reflection::Type ${adapterName}::GetType(soia_type<type>) {`,
      );
      const recordId = `${struct.record.name.text}:${struct.modulePath}`;
      source.internalMain.push(
        `  return soia::reflection::RecordType({"${recordId}"});`,
      );
      source.internalMain.push("}");
      source.internalMain.push("");
    }

    {
      // RegisterRecords(soia_type<T>, RecordRegistry&)
      const { modulePath } = struct;
      const recordName = struct.record.name.text;
      const recordId = `${modulePath}:${recordName}`;
      source.internalMain.push(`void ${adapterName}::RegisterRecords(`);
      source.internalMain.push("    soia_type<type>,");
      source.internalMain.push(
        "    soia::reflection::RecordRegistry& registry) {",
      );
      source.internalMain.push("  const bool already_present =");
      source.internalMain.push(
        `      registry.find_or_null("${recordId}") != nullptr;`,
      );
      source.internalMain.push("  if (already_present) return;");
      source.internalMain.push("  soia::reflection::Record record = {");
      source.internalMain.push("      soia::reflection::RecordKind::kStruct,");
      source.internalMain.push(`      "${recordId}",`);
      source.internalMain.push("      {");
      for (const field of fields) {
        const ccType = typeSpeller.getCcType(field.type!, {
          forceNamespace: true,
        });
        source.internalMain.push("          {");
        source.internalMain.push(`              "${field.name.text}",`);
        source.internalMain.push(
          `              soia_internal::GetType<${ccType}>(),`,
        );
        source.internalMain.push(`              ${field.number},`);
        source.internalMain.push("          },");
      }
      source.internalMain.push("      },");
      const removedNumbers = struct.record.removedNumbers.join(", ");
      source.internalMain.push(`      {${removedNumbers}},`);
      source.internalMain.push("  };");
      source.internalMain.push("  registry.push_back(std::move(record));");
      for (const field of fields) {
        const ccType = typeSpeller.getCcType(field.type!, {
          forceNamespace: true,
        });
        source.internalMain.push(
          `  soia_internal::RegisterRecords<${ccType}>(registry);`,
        );
      }
      source.internalMain.push("}");
      source.internalMain.push("");
    }

    {
      // Struct::operator==(const T&)
      source.mainBottom.push(
        `bool ${className}::operator==(${constRefType} other) const {`,
      );
      const expression = fields.length
        ? fields
            .map((f) => {
              const fieldName = maybeEscapeLowerCaseName(f.name.text);
              return `this->${fieldName} == other.${fieldName}`;
            })
            .join("\n      && ")
        : "true";
      source.mainBottom.push(`  return ${expression};`);
      source.mainBottom.push("}");
      source.mainBottom.push("");
    }

    {
      // Whole -> Struct
      source.mainBottom.push(`${className}::whole::operator ${className}() {`);
      source.mainBottom.push(`  return ${className}{`);
      for (const field of fieldsByName) {
        const fieldName = maybeEscapeLowerCaseName(field.name.text);
        source.mainBottom.push(`      *std::move(${fieldName}),`);
      }
      source.mainBottom.push("  };");
      source.mainBottom.push("}");
      source.mainBottom.push("");
    }
  }

  private writeCodeForEnum(record: RecordLocation): void {
    const { header, recordMap, source, typeSpeller } = this;

    const { nestedRecords } = record.record;
    const fields = getEnumFields(record.record.fields, this.typeSpeller);
    const constFields = fields.filter((f) => !f.valueType);
    const valueFields = fields.filter((f) => f.valueType);
    const pointerFields = valueFields.filter((f) => f.usePointer);

    for (const field of constFields) {
      this.writeCodeForConstantField(field);
    }
    for (const field of valueFields) {
      this.writeCodeForValueField(field);
    }

    const className = getClassName(record);
    const adapterName = `${className}Adapter`;
    const qualifiedName = `::${this.namespace}::${className}`;
    const constRefType = `const ${qualifiedName}&`;

    header.mainTop.push(`class ${className};`);

    header.mainMiddle.push(`class ${className} {`);
    header.mainMiddle.push(" public:");
    for (const field of valueFields) {
      const type = `::soiagen::${field.structType}<${field.valueType}>`;
      header.mainMiddle.push(`  using ${field.typeAlias} = ${type};`);
    }
    header.mainMiddle.push("");
    header.mainMiddle.push("  enum class kind_type {");
    for (const field of fields) {
      header.mainMiddle.push(`    ${field.kindEnumerator},`);
    }
    header.mainMiddle.push("  };");
    header.mainMiddle.push("");
    header.mainMiddle.push(`  ${className}();`);
    source.mainMiddle.push(
      `${className}::${className}() : ${className}(kUnknown) {}`,
    );
    source.mainMiddle.push("");
    header.mainMiddle.push(`  ${className}(const ${className}&);`);
    source.mainMiddle.push(
      `${className}::${className}(const ${className}& other) {`,
    );
    source.mainMiddle.push("  copy(other);");
    source.mainMiddle.push("}");
    source.mainMiddle.push("");

    header.mainMiddle.push(`  ${className}(${className}&&);`);
    source.mainMiddle.push(`${className}::${className}(${className}&& other)`);
    source.mainMiddle.push(`    : kind_(other.kind_),`);
    source.mainMiddle.push(`      value_(other.value_) {`);
    source.mainMiddle.push("  other.kind_ = kind_type::kConstUnknown;");
    source.mainMiddle.push("  other.value_._unrecognized = nullptr;");
    source.mainMiddle.push("}");
    source.mainMiddle.push("");
    header.mainMiddle.push("");
    for (const field of constFields) {
      const { isUnknownField, kindEnumerator, structType } = field;
      header.mainMiddle.push(`  ${className}(::soiagen::${structType});`);
      const body = isUnknownField
        ? "{\n  value_._unrecognized = nullptr;\n}"
        : "{}";
      source.mainMiddle.push(
        `${className}::${className}(::soiagen::${
          structType
        }) : kind_(kind_type::${kindEnumerator}) ${body}`,
      );
    }
    source.mainMiddle.push("");
    for (const field of valueFields) {
      const { fieldName, kindEnumerator, typeAlias, usePointer } = field;
      header.mainMiddle.push(`  ${className}(${typeAlias});`);
      source.mainMiddle.push(`${className}::${className}(${typeAlias} w)`);
      source.mainMiddle.push(`    : kind_(kind_type::${kindEnumerator}) {`);
      if (usePointer) {
        source.mainMiddle.push(
          `  value_.${fieldName}_ = new ${typeAlias}(std::move(w));`,
        );
      } else {
        source.mainMiddle.push(`  value_.${fieldName}_ = w;`);
      }
      source.mainMiddle.push("}");
      source.mainMiddle.push("");
    }
    {
      source.mainMiddle.push(
        `${className}::${
          className
        }(unrecognized_enum u) : kind_(kind_type::kConstUnknown) {`,
      );
      source.mainMiddle.push(
        "  value_._unrecognized = new unrecognized_enum(std::move(u));",
      );
      source.mainMiddle.push("}");
      source.mainMiddle.push("");
    }
    header.mainMiddle.push("");
    header.mainMiddle.push(`  ~${className}();`);
    header.mainMiddle.push("");
    source.mainMiddle.push(`${className}::~${className}() {`);
    source.mainMiddle.push("  free_value();");
    source.mainMiddle.push("}");
    source.mainMiddle.push("");
    for (const field of constFields) {
      const { identifier, valueType } = field;
      header.mainMiddle.push(
        `  static constexpr auto ${identifier} = ::soiagen::${identifier};`,
      );
    }
    header.mainMiddle.push("");
    for (const field of valueFields) {
      const { identifier, usePointer, valueType } = field;
      header.mainMiddle.push(
        `  static ${className} ${identifier}(${valueType} value);`,
      );
      source.mainMiddle.push(
        `${className} ${className}::${identifier}(${valueType} value) {`,
      );
      const maybeMoveValue = usePointer ? "std::move(value)" : "value";
      const returnValue = `${className}(::soiagen::${identifier}(${maybeMoveValue}))`;
      source.mainMiddle.push(`  return ${returnValue};`);
      source.mainMiddle.push("}");
      source.mainMiddle.push("");
    }
    header.mainMiddle.push("");
    header.mainMiddle.push("  kind_type kind() const { return kind_; }");
    header.mainMiddle.push("");
    for (const field of valueFields) {
      const { fieldName, kindEnumerator, usePointer, valueType } = field;
      header.mainMiddle.push(`  inline bool is_${fieldName}() const;`);
      header.mainMiddle.push(
        `  inline const ${valueType}& as_${fieldName}() const;`,
      );
      header.mainMiddle.push(`  inline ${valueType}& as_${fieldName}();`);
      header.mainMiddle.push("");
      header.mainBottom.push(
        `inline bool ${className}::is_${fieldName}() const {`,
      );
      header.mainBottom.push(`  return kind_ == kind_type::${kindEnumerator};`),
        header.mainBottom.push("}");
      header.mainBottom.push("");
      header.mainBottom.push(
        `inline const ${valueType}& ${className}::as_${fieldName}() const {`,
      );
      header.mainBottom.push(
        `  return const_cast<${className}*>(this)->as_${fieldName}();`,
      );
      header.mainBottom.push("}");
      header.mainBottom.push("");
      header.mainBottom.push(
        `inline ${valueType}& ${className}::as_${fieldName}() {`,
      );
      header.mainBottom.push(
        `  ABSL_CHECK(is_${fieldName}()) << "actual: " << *this;`,
      );
      const returnValue = usePointer
        ? `value_.${fieldName}_->value`
        : `value_.${fieldName}_.value`;
      header.mainBottom.push(`  return ${returnValue};`);
      header.mainBottom.push("}");
      header.mainBottom.push("");
    }

    header.mainMiddle.push("  template <typename Visitor>");
    header.mainMiddle.push("  decltype(auto) visit(Visitor&& visitor) const {");
    header.mainMiddle.push(
      `    return visit_impl(*this, std::forward<Visitor>(visitor));`,
    );
    header.mainMiddle.push("  }");
    header.mainMiddle.push("  template <typename Visitor>");
    header.mainMiddle.push("  decltype(auto) visit(Visitor&& visitor) {");
    header.mainMiddle.push(
      `    return visit_impl(*this, std::forward<Visitor>(visitor));`,
    );
    header.mainMiddle.push("  }");
    header.mainMiddle.push("");

    header.mainMiddle.push(
      `  ${className}& operator=(const ${className}& other);`,
    );
    source.mainMiddle.push(
      `${className}& ${className}::operator=(const ${className}& other) {`,
    );
    source.mainMiddle.push("  free_value();");
    source.mainMiddle.push("  copy(other);");
    source.mainMiddle.push("  return *this;");
    source.mainMiddle.push("}");
    source.mainMiddle.push("");
    header.mainMiddle.push(`  ${className}& operator=(${className}&& other);`);
    source.mainMiddle.push(
      `${className}& ${className}::operator=(${className}&& other) {`,
    );
    source.mainMiddle.push("  free_value();");
    source.mainMiddle.push("  kind_ = other.kind_;");
    source.mainMiddle.push("  value_ = other.value_;");
    source.mainMiddle.push("  other.kind_ = kind_type::kConstUnknown;");
    source.mainMiddle.push("  other.value_._unrecognized = nullptr;");
    source.mainMiddle.push("  return *this;");
    source.mainMiddle.push("}");
    source.mainMiddle.push("");
    header.mainMiddle.push("");
    for (const field of constFields) {
      const { isUnknownField, kindEnumerator, structType } = field;
      header.mainMiddle.push(
        `  ${className}& operator=(::soiagen::${structType});`,
      );
      source.mainMiddle.push(
        `${className}& ${className}::operator=(::soiagen::${structType}) {`,
      );
      source.mainMiddle.push("  free_value();");
      source.mainMiddle.push(`  kind_ = kind_type::${kindEnumerator};`);
      if (isUnknownField) {
        source.mainMiddle.push("  value_._unrecognized = nullptr;");
      }
      source.mainMiddle.push("  return *this;");
      source.mainMiddle.push("}");
      source.mainMiddle.push("");
    }
    for (const field of valueFields) {
      const { fieldName, kindEnumerator, typeAlias, usePointer } = field;
      header.mainMiddle.push(`  ${className}& operator=(${typeAlias});`);
      source.mainMiddle.push(
        `${className}& ${className}::operator=(${typeAlias} w) {`,
      );
      source.mainMiddle.push("  free_value();");
      source.mainMiddle.push(`  kind_ = kind_type::${kindEnumerator};`);
      if (usePointer) {
        source.mainMiddle.push(
          `  value_.${fieldName}_ = new ${typeAlias}(std::move(w));`,
        );
      } else {
        source.mainMiddle.push(`  value_.${fieldName}_ = w;`);
      }
      source.mainMiddle.push("  return *this;");
      source.mainMiddle.push("}");
      source.mainMiddle.push("");
    }
    header.mainMiddle.push("");

    if (valueFields.length) {
      header.mainMiddle.push(`  bool operator==(const ${className}&) const;`);

      source.mainMiddle.push(
        `bool ${className}::operator==(const ${className}& other) const {`,
      );
      source.mainMiddle.push("  if (other.kind_ != kind_) return false;");
      source.mainMiddle.push("  switch (kind_) {");
      for (const field of valueFields) {
        const { fieldName, kindEnumerator, usePointer } = field;
        const dotOrStar = usePointer ? "->" : ".";
        const a = `value_.${fieldName}_${dotOrStar}value`;
        const b = `other.value_.${fieldName}_${dotOrStar}value`;
        source.mainMiddle.push(`    case kind_type::${kindEnumerator}:`);
        source.mainMiddle.push(`      return ${a} == ${b};`);
      }
      source.mainMiddle.push(`    default:`);
      source.mainMiddle.push("      return true;");
      source.mainMiddle.push("  }");
      source.mainMiddle.push("}");
      source.mainMiddle.push("");
    } else {
      header.mainMiddle.push(
        `  inline bool operator==(const ${className}& other) const {`,
      );
      header.mainMiddle.push("    return other.kind_ == kind_;");
      header.mainMiddle.push("  }");
    }
    header.mainMiddle.push("");
    header.mainMiddle.push(
      `  inline bool operator!=(const ${className}& other) const {`,
    );
    header.mainMiddle.push("    return !(*this == other);");
    header.mainMiddle.push("  }");
    header.mainMiddle.push("");
    for (const nestedRecord of nestedRecords) {
      let typeAlias = nestedRecord.name.text;
      if (typeAlias === className) {
        typeAlias = `${typeAlias}_`;
      }
      const recordLocation = recordMap.get(nestedRecord.key)!;
      const nestedClassName = getClassName(recordLocation);
      header.mainMiddle.push(`  using ${typeAlias} = ${nestedClassName};`);
    }
    header.mainMiddle.push("");
    header.mainMiddle.push(" private:");
    header.mainMiddle.push(
      "  using unrecognized_enum = ::soia_internal::UnrecognizedEnum;",
    );
    header.mainMiddle.push("");
    header.mainMiddle.push(`  ${className}(unrecognized_enum);`);
    header.mainMiddle.push("");
    header.mainMiddle.push("  kind_type kind_;");
    header.mainMiddle.push("");
    header.mainMiddle.push("  union value_wrapper {");
    header.mainMiddle.push("    value_wrapper() {}");
    header.mainMiddle.push("    unrecognized_enum* _unrecognized;");
    for (const field of valueFields) {
      const { fieldName, typeAlias } = field;
      const maybeStar = field.usePointer ? "*" : "";
      header.mainMiddle.push(`    ${typeAlias}${maybeStar} ${fieldName}_;`);
    }
    header.mainMiddle.push("  };");
    header.mainMiddle.push("  value_wrapper value_;");
    header.mainMiddle.push("");
    header.mainMiddle.push(`  void copy(const ${className}&);`);

    source.mainMiddle.push(
      `void ${className}::copy(const ${className}& other) {`,
    );
    source.mainMiddle.push("  kind_ = other.kind_;");
    source.mainMiddle.push("  switch (other.kind_) {");
    source.mainMiddle.push("    case kind_type::kConstUnknown: {");
    source.mainMiddle.push(
      "      const unrecognized_enum* u = other.value_._unrecognized;",
    );
    source.mainMiddle.push(
      "      value_._unrecognized = u != nullptr ? new unrecognized_enum(*u) : nullptr;",
    );
    source.mainMiddle.push("      break;");
    source.mainMiddle.push("    }");
    for (const field of valueFields) {
      const { fieldName, kindEnumerator, typeAlias, usePointer } = field;
      source.mainMiddle.push(`    case kind_type::${kindEnumerator}:`);
      if (usePointer) {
        const expr = `new ${typeAlias}(*other.value_.${fieldName}_)`;
        source.mainMiddle.push(`      value_.${fieldName}_ = ${expr};`);
      } else {
        source.mainMiddle.push(
          `      value_.${fieldName}_ = other.value_.${fieldName}_;`,
        );
      }
      source.mainMiddle.push("      break;");
    }
    source.mainMiddle.push("    default:");
    source.mainMiddle.push("      break;");
    source.mainMiddle.push("  }");
    source.mainMiddle.push("}");
    source.mainMiddle.push("");
    header.mainMiddle.push("  void free_value() const;");

    source.mainMiddle.push(`void ${className}::free_value() const {`);
    source.mainMiddle.push("  switch (kind_) {");
    source.mainMiddle.push("    case kind_type::kConstUnknown:");
    source.mainMiddle.push(
      "      ::std::unique_ptr<unrecognized_enum>(value_._unrecognized);",
    );
    source.mainMiddle.push("      break;");
    for (const field of pointerFields) {
      const { fieldName, kindEnumerator, typeAlias } = field;
      source.mainMiddle.push(`    case kind_type::${kindEnumerator}:`);
      source.mainMiddle.push(
        `      ::std::unique_ptr<${typeAlias}>(value_.${fieldName}_);`,
      );
      source.mainMiddle.push("      break;");
    }
    source.mainMiddle.push("    default:");
    source.mainMiddle.push("      break;");
    source.mainMiddle.push("  }");
    source.mainMiddle.push("}");
    source.mainMiddle.push("");
    header.mainMiddle.push("");
    header.mainMiddle.push("  template <typename E, typename Visitor>");
    header.mainMiddle.push(
      "  static decltype(auto) visit_impl(E& e, Visitor&& visitor) {",
    );
    header.mainMiddle.push("    switch (e.kind_) {");
    for (const field of constFields) {
      const { kindEnumerator, structType } = field;
      header.mainMiddle.push(`      case kind_type::${kindEnumerator}:`);
      header.mainMiddle.push(
        `        return std::forward<Visitor>(visitor)(::soiagen::${
          structType
        }());`,
      );
    }
    for (const field of valueFields) {
      const { fieldName, kindEnumerator, usePointer } = field;
      const maybeStar = usePointer ? "*" : "";
      header.mainMiddle.push(`      case kind_type::${kindEnumerator}:`);
      header.mainMiddle.push(
        `        return std::forward<Visitor>(visitor)(${maybeStar}e.value_.${
          fieldName
        }_);`,
      );
    }
    header.mainMiddle.push("    }");
    header.mainMiddle.push("    ABSL_CHECK(false);");
    header.mainMiddle.push("  }");
    header.mainMiddle.push("");
    header.mainMiddle.push(
      `  friend class ::soia_internal::${this.namespace}::${adapterName};`,
    );
    header.mainMiddle.push("};");
    header.mainMiddle.push("");

    for (const field of constFields) {
      const { kindEnumerator, structType } = field;
      header.mainBottom.push("inline bool operator==(");
      header.mainBottom.push(`    ${constRefType} a,`);
      header.mainBottom.push(`    ::soiagen::${structType}) {`);
      header.mainBottom.push(
        `  return a.kind() == ${qualifiedName}::kind_type::${kindEnumerator};`,
      );
      header.mainBottom.push("}");
      header.mainBottom.push("");
      header.mainBottom.push("inline bool operator!=(");
      header.mainBottom.push(`    ${constRefType} a,`);
      header.mainBottom.push(`    ::soiagen::${structType} b) {`);
      header.mainBottom.push("  return !(a == b);");
      header.mainBottom.push("}");
      header.mainBottom.push("");
      header.mainBottom.push("inline bool operator==(");
      header.mainBottom.push(`    ::soiagen::${field.structType},`);
      header.mainBottom.push(`    ${constRefType} b) {`);
      header.mainBottom.push(
        `  return ${qualifiedName}::kind_type::${kindEnumerator} == b.kind();`,
      );
      header.mainBottom.push("}");
      header.mainBottom.push("");
      header.mainBottom.push("inline bool operator!=(");
      header.mainBottom.push(`    ::soiagen::${field.structType} a,`);
      header.mainBottom.push(`    ${constRefType} b) {`);
      header.mainBottom.push("  return !(a == b);");
      header.mainBottom.push("}");
      header.mainBottom.push("");
    }
    header.mainBottom.push("template <typename H>");
    header.mainBottom.push(`H AbslHashValue(H h, ${constRefType} input) {`);
    header.mainBottom.push("  struct visitor {");
    header.mainBottom.push("    H h;");
    for (const field of constFields) {
      const { fieldName, structType } = field;
      header.mainBottom.push(`    H operator()(::soiagen::${structType}) {`);
      const hash = simpleHash(fieldName);
      header.mainBottom.push(`      return H::combine(std::move(h), ${hash});`);
      header.mainBottom.push("    }");
    }
    for (const field of valueFields) {
      const { fieldName, typeAlias } = field;
      header.mainBottom.push(
        `    H operator()(const ${className}::${typeAlias}& w) {`,
      );
      const hash = simpleHash(fieldName);
      header.mainBottom.push(
        `      return H::combine(std::move(h), ${hash}, w.value);`,
      );
      header.mainBottom.push("    }");
    }
    header.mainBottom.push("  };");
    header.mainBottom.push("  return input.visit(visitor{std::move(h)});");
    header.mainBottom.push("}");
    header.mainBottom.push("");
    header.mainMiddle.push("inline std::ostream& operator<<(");
    header.mainMiddle.push("    std::ostream& os,");
    header.mainMiddle.push(`    ${constRefType} input) {`);
    header.mainMiddle.push(
      "  return os << ::soia_internal::ToDebugString(input);",
    );
    header.mainMiddle.push("}");
    header.mainMiddle.push("");

    {
      // IsDefault(const T&)
      source.internalMain.push(
        `bool ${adapterName}::IsDefault(const type& input) {`,
      );
      source.internalMain.push("  return input == ::soiagen::kUnknown;");
      source.internalMain.push("}");
      source.internalMain.push("");
    }

    {
      // Append(const T&, DenseJson&)
      source.internalMain.push(
        `void ${adapterName}::Append(const type& input, DenseJson& out) {`,
      );
      source.internalMain.push("  switch (input.kind_) {");
      for (const field of constFields) {
        const { fieldNumber, kindEnumerator, isUnknownField } = field;
        source.internalMain.push(
          `    case type::kind_type::${kindEnumerator}: {`,
        );
        if (isUnknownField) {
          source.internalMain.push(
            "      if (input.value_._unrecognized != nullptr) {",
          );
          source.internalMain.push(
            "        AppendUnrecognizedEnum(*input.value_._unrecognized, out);",
          );
          source.internalMain.push("      } else {");
          source.internalMain.push("        out.out += '0';");
          source.internalMain.push("      }");
        } else {
          source.internalMain.push(
            `      out.out += {${numberToCharLiterals(fieldNumber)}};`,
          );
        }
        source.internalMain.push("      break;");
        source.internalMain.push("    }");
      }
      for (const field of valueFields) {
        const { fieldName, fieldNumber, kindEnumerator, usePointer } = field;
        const dotOrArrow = usePointer ? "->" : ".";
        source.internalMain.push(
          `    case type::kind_type::${kindEnumerator}: {`,
        );
        source.internalMain.push(
          `      out.out += {'[', ${numberToCharLiterals(fieldNumber)}, ','};`,
        );
        source.internalMain.push(
          `      ::soia_internal::Append(input.value_.${fieldName}_${
            dotOrArrow
          }value, out);`,
        );
        source.internalMain.push("      out.out += ']';");
        source.internalMain.push("      break;");
        source.internalMain.push("    }");
      }
      source.internalMain.push("  }");
      source.internalMain.push("}");
      source.internalMain.push("");
    }

    {
      // Append(const T&, ReadableJson&)
      source.internalMain.push(
        `void ${adapterName}::Append(const type& input, ReadableJson& out) {`,
      );
      source.internalMain.push("  struct visitor {");
      source.internalMain.push("    ReadableJson& out;");
      for (const field of constFields) {
        const { fieldName, structType } = field;
        source.internalMain.push(
          `    void operator()(::soiagen::${structType}) {`,
        );
        source.internalMain.push(`      out.out += "\\"${fieldName}\\"";`);
        source.internalMain.push("    }");
      }
      for (const field of valueFields) {
        const { fieldName, typeAlias } = field;
        source.internalMain.push(
          `    void operator()(const type::${typeAlias}& w) {`,
        );
        source.internalMain.push("      out.new_line.Indent();");
        source.internalMain.push(
          `      absl::StrAppend(&out.out, "{", *out.new_line, "\\"kind\\": \\"${fieldName}\\",",`,
        );
        source.internalMain.push(
          '                      *out.new_line, "\\"value\\": ");',
        );
        source.internalMain.push(
          "      ::soia_internal::Append(w.value, out);",
        );
        source.internalMain.push(
          '      absl::StrAppend(&out.out, out.new_line.Dedent(), "}");',
        );
        source.internalMain.push("    }");
      }
      source.internalMain.push("  };");
      source.internalMain.push("  input.visit(visitor{out});");
      source.internalMain.push("}");
      source.internalMain.push("");
    }

    {
      // Append(const T&, DebugString&)
      source.internalMain.push(
        `void ${adapterName}::Append(const type& input, DebugString& out) {`,
      );
      source.internalMain.push("  struct visitor {");
      source.internalMain.push("    DebugString& out;");
      for (const field of constFields) {
        const { identifier, structType } = field;
        source.internalMain.push(
          `    void operator()(::soiagen::${structType}) {`,
        );
        source.internalMain.push(`      out.out += "soiagen::${identifier}";`);
        source.internalMain.push("    }");
      }
      for (const field of valueFields) {
        const { identifier, typeAlias } = field;
        source.internalMain.push(
          `    void operator()(const ${qualifiedName}::${typeAlias}& w) {`,
        );
        source.internalMain.push(
          `      out.out += "::soiagen::${identifier}(";`,
        );
        source.internalMain.push(
          "      ::soia_internal::Append(w.value, out);",
        );
        source.internalMain.push("      out.out += ')';");
        source.internalMain.push("    }");
      }
      source.internalMain.push("  };");
      source.internalMain.push("  input.visit(visitor{out});");
      source.internalMain.push("}");
      source.internalMain.push("");
    }

    {
      // Append(const T&, ByteSink&)
      source.internalMain.push(
        `void ${adapterName}::Append(const type& input, ByteSink& out) {`,
      );
      source.internalMain.push("  switch (input.kind_) {");
      for (const field of constFields) {
        const { fieldNumber, isUnknownField, kindEnumerator } = field;
        source.internalMain.push(
          `    case type::kind_type::${kindEnumerator}: {`,
        );
        if (isUnknownField) {
          source.internalMain.push(
            "      if (input.value_._unrecognized != nullptr) {",
          );
          source.internalMain.push(
            "        AppendUnrecognizedEnum(*input.value_._unrecognized, out);",
          );
          source.internalMain.push("      } else {");
          source.internalMain.push("        out.Push(0);");
          source.internalMain.push("      }");
        } else {
          const intLiterals = bytesToIntLiterals([...encodeInt32(fieldNumber)]);
          source.internalMain.push(`      out.Push(${intLiterals});`);
        }
        source.internalMain.push("      break;");
        source.internalMain.push("    }");
      }
      for (const field of valueFields) {
        const { fieldName, fieldNumber, kindEnumerator, usePointer } = field;
        const intLiterals = bytesToIntLiterals(
          1 <= fieldNumber && fieldNumber <= 4
            ? [fieldNumber + 250]
            : [248, ...encodeInt32(fieldNumber)],
        );
        const dotOrArrow = usePointer ? "->" : ".";
        source.internalMain.push(
          `    case type::kind_type::${kindEnumerator}: {`,
        );
        source.internalMain.push(`      out.Push(${intLiterals});`);
        source.internalMain.push(
          `      ::soia_internal::Append(input.value_.${fieldName}_${
            dotOrArrow
          }value, out);`,
        );
        source.internalMain.push("      break;");
        source.internalMain.push("    }");
      }
      source.internalMain.push("  }");
      source.internalMain.push("}");
      source.internalMain.push("");
    }

    {
      // Parse(JsonTokenizer&, T&)
      source.internalMain.push(
        `void ${adapterName}::Parse(JsonTokenizer& tokenizer, type& out) {`,
      );
      source.internalMain.push("  switch (tokenizer.state().token_type) {");
      source.internalMain.push("    case JsonTokenType::kZero:");
      source.internalMain.push("      tokenizer.Next();");
      source.internalMain.push("      break;");
      source.internalMain.push("    case JsonTokenType::kUnsignedInteger: {");
      source.internalMain.push(
        "      const int i = tokenizer.state().uint_value;",
      );
      source.internalMain.push("      switch (i) {");
      for (const field of constFields) {
        const { fieldNumber, identifier } = field;
        if (field.fieldNumber <= 0) continue;
        source.internalMain.push(`        case ${fieldNumber}:`);
        source.internalMain.push(`          out = ::soiagen::${identifier};`);
        source.internalMain.push("          break;");
      }
      source.internalMain.push("        default:");
      source.internalMain.push("          out = type(UnrecognizedEnum{i});");
      source.internalMain.push("      }");
      source.internalMain.push("      tokenizer.Next();");
      source.internalMain.push("      break;");
      source.internalMain.push("    }");
      source.internalMain.push("    case JsonTokenType::kSignedInteger: {");
      source.internalMain.push(
        "      const int i = tokenizer.state().int_value;",
      );
      source.internalMain.push("      switch (i) {");
      for (const field of constFields) {
        const { fieldNumber, identifier } = field;
        if (0 <= field.fieldNumber) continue;
        source.internalMain.push(`        case ${fieldNumber}:`);
        source.internalMain.push(`          out = ::soiagen::${identifier};`);
        source.internalMain.push("          break:");
      }
      source.internalMain.push("        default:");
      source.internalMain.push("          out = type(UnrecognizedEnum{i});");
      source.internalMain.push("      }");
      source.internalMain.push("      tokenizer.Next();");
      source.internalMain.push("      break;");
      source.internalMain.push("    }");
      source.internalMain.push("    case JsonTokenType::kString: {");
      source.internalMain.push(
        "      static const auto* kMap = new ::absl::flat_hash_map<std::string, type>({",
      );
      for (const field of constFields) {
        const { fieldName, identifier } = field;
        source.internalMain.push(
          `          {"${fieldName}", type::${identifier}},`,
        );
      }
      source.internalMain.push("      });");
      source.internalMain.push(
        "      const auto it = kMap->find(tokenizer.state().string_value);",
      );
      source.internalMain.push("      if (it == kMap->cend()) break;");
      source.internalMain.push("      out = it->second;");
      source.internalMain.push("      tokenizer.Next();");
      source.internalMain.push("      break;");
      source.internalMain.push("    }");
      source.internalMain.push("    case JsonTokenType::kLeftSquareBracket: {");
      source.internalMain.push("      EnumJsonArrayParser parser(&tokenizer);");
      source.internalMain.push("      const int number = parser.ReadNumber();");
      source.internalMain.push("      switch (number) {");
      for (const field of valueFields) {
        const { fieldNumber, typeAlias } = field;
        source.internalMain.push(`        case ${fieldNumber}: {`);
        source.internalMain.push(`          type::${typeAlias} wrapper;`);
        source.internalMain.push(
          "          ::soia_internal::Parse(tokenizer, wrapper.value);",
        );
        source.internalMain.push("          out = std::move(wrapper);");
        source.internalMain.push("          break;");
        source.internalMain.push("        }");
      }
      source.internalMain.push(`        default: {`);
      source.internalMain.push(
        "          UnrecognizedEnum unrecognized{number};",
      );
      source.internalMain.push(
        "          unrecognized.emplace_value().ParseFrom(tokenizer);",
      );
      source.internalMain.push(
        "          out = type(std::move(unrecognized));",
      );
      source.internalMain.push("        }");
      source.internalMain.push("      }");
      source.internalMain.push("      parser.Finish();");
      source.internalMain.push("      break;");
      source.internalMain.push("    }");
      source.internalMain.push("    case JsonTokenType::kLeftCurlyBracket: {");
      const parserExpr =
        "(new EnumJsonObjectParser<type>())" +
        valueFields
          .map((field) => {
            const { fieldName, typeAlias } = field;
            const indent = "              ";
            return `\n${indent}->AddField<type::${typeAlias}>("${fieldName}")`;
          })
          .join("");
      source.internalMain.push("      static const auto* kParser =");
      source.internalMain.push(`          ${parserExpr};`);
      source.internalMain.push("      kParser->Parse(tokenizer, out);");
      source.internalMain.push("      break;");
      source.internalMain.push("    }");
      source.internalMain.push("    default: {");
      source.internalMain.push(
        "      tokenizer.mutable_state().PushUnexpectedTokenError(\"number or '['\");",
      );
      source.internalMain.push("    }");
      source.internalMain.push("  }");
      source.internalMain.push("}");
      source.internalMain.push("");
    }

    {
      // Parse(ByteSource&, T&)
      source.internalMain.push(
        `void ${adapterName}::Parse(ByteSource& source, type& out) {`,
      );
      source.internalMain.push(
        "  const auto [has_value, number] = ParseEnumPrefix(source);",
      );
      source.internalMain.push("  if (has_value) {");
      source.internalMain.push("    switch (number) {");
      for (const field of valueFields) {
        const { fieldNumber, typeAlias } = field;
        source.internalMain.push(`      case ${fieldNumber}: {`);
        source.internalMain.push(`        type::${typeAlias} wrapper;`);
        source.internalMain.push(
          "        ::soia_internal::Parse(source, wrapper.value);",
        );
        source.internalMain.push("        out = std::move(wrapper);");
        source.internalMain.push("        break;");
        source.internalMain.push("      }");
      }
      source.internalMain.push("      default: {");
      source.internalMain.push(
        "        UnrecognizedEnum unrecognized{number};",
      );
      source.internalMain.push(
        "        unrecognized.emplace_value().ParseFrom(source);",
      );
      source.internalMain.push("        out = type(std::move(unrecognized));");
      source.internalMain.push("      }");
      source.internalMain.push("    }");
      source.internalMain.push("  } else {");
      source.internalMain.push("    switch (number) {");
      source.internalMain.push("      case 0:");
      source.internalMain.push("        break;");
      for (const field of constFields) {
        const { fieldNumber, identifier } = field;
        if (field.fieldNumber === 0) continue;
        source.internalMain.push(`      case ${fieldNumber}:`);
        source.internalMain.push(`        out = ::soiagen::${identifier};`);
        source.internalMain.push("        break;");
      }
      source.internalMain.push("      default: {");
      source.internalMain.push("        out = type(UnrecognizedEnum{number});");
      source.internalMain.push("      }");
      source.internalMain.push("    }");
      source.internalMain.push("  }");
      source.internalMain.push("}");
      source.internalMain.push("");
    }

    {
      // GetType(soia_type<T>)
      source.internalMain.push(
        `soia::reflection::Type ${adapterName}::GetType(soia_type<type>) {`,
      );
      const recordId = `${record.record.name.text}:${record.modulePath}`;
      source.internalMain.push(
        `  return soia::reflection::RecordType({"${recordId}"});`,
      );
      source.internalMain.push("}");
      source.internalMain.push("");
    }

    {
      // RegisterRecords(soia_type<T>, RecordRegistry&)
      const { modulePath } = record;
      const recordName = record.record.name.text;
      const recordId = `${modulePath}:${recordName}`;
      source.internalMain.push(`void ${adapterName}::RegisterRecords(`);
      source.internalMain.push("    soia_type<type>,");
      source.internalMain.push(
        "    soia::reflection::RecordRegistry& registry) {",
      );
      source.internalMain.push("  const bool already_present =");
      source.internalMain.push(
        `      registry.find_or_null("${recordId}") != nullptr;`,
      );
      source.internalMain.push("  if (already_present) return;");
      source.internalMain.push("  soia::reflection::Record record = {");
      source.internalMain.push("      soia::reflection::RecordKind::kEnum,");
      source.internalMain.push(`      "${recordId}",`);
      source.internalMain.push("      {");
      for (const field of constFields) {
        source.internalMain.push("          {");
        source.internalMain.push(`              "${field.fieldName}",`);
        source.internalMain.push(`              absl::nullopt,`);
        source.internalMain.push(`              ${field.fieldNumber},`);
        source.internalMain.push("          },");
      }
      for (const field of valueFields) {
        const { fieldName, fieldNumber, valueTypeWithNamespace } = field;
        source.internalMain.push("          {");
        source.internalMain.push(`              "${fieldName}",`);
        source.internalMain.push(
          `              soia_internal::GetType<${valueTypeWithNamespace}>(),`,
        );
        source.internalMain.push(`              ${fieldNumber},`);
        source.internalMain.push("          },");
      }
      source.internalMain.push("      },");
      const removedNumbers = record.record.removedNumbers.join(", ");
      source.internalMain.push(`      {${removedNumbers}},`);
      source.internalMain.push("  };");
      source.internalMain.push("  registry.push_back(std::move(record));");
      for (const field of valueFields) {
        const { valueTypeWithNamespace } = field;
        source.internalMain.push(
          `  soia_internal::RegisterRecords<${
            valueTypeWithNamespace
          }>(registry);`,
        );
      }
      source.internalMain.push("}");
      source.internalMain.push("");
    }
  }

  private writeCodeInHeaderForAdapter(record: RecordLocation) {
    const { header } = this;

    const { fields, recordType } = record.record;
    const className = getClassName(record);
    const adapterName = `${className}Adapter`;
    const qualifiedName = `::${this.namespace}::${className}`;

    header.internalMainTop.push(`class ${adapterName};`);
    header.internalMain.push(`class ${adapterName} {`);
    header.internalMain.push(" public:");
    header.internalMain.push(`  using type = ${qualifiedName};`);
    if (fields.length || recordType === "enum") {
      function fieldToReflectionType(f: Field): string {
        const fieldName = f.name.text;
        if (recordType === "struct") {
          return `struct_field<type, soiagen::get_${fieldName}<>>`;
        } else if (f.type) {
          return `enum_value_field<type, soiagen::reflection::${fieldName}_option>`;
        } else {
          return `soia::reflection::enum_const_field<soiagen::k_${fieldName.toLowerCase()}>`;
        }
      }
      const reflectionTypes = fields
        .map(fieldToReflectionType)
        .concat(
          recordType === "enum"
            ? ["soia::reflection::enum_const_field<soiagen::k_unknown>"]
            : [],
        )
        .join(",\n      ");
      header.internalMain.push(
        `  using fields_tuple = std::tuple<\n      ${reflectionTypes}>;`,
      );
    } else {
      header.internalMain.push("  using fields_tuple = std::tuple<>;");
    }
    header.internalMain.push("");
    header.internalMain.push("  static bool IsDefault(const type&);");
    header.internalMain.push("  static void Append(const type&, DenseJson&);");
    header.internalMain.push(
      "  static void Append(const type&, ReadableJson&);",
    );
    header.internalMain.push(
      "  static void Append(const type&, DebugString&);",
    );
    header.internalMain.push("  static void Append(const type&, ByteSink&);");
    header.internalMain.push("  static void Parse(JsonTokenizer&, type&);");
    header.internalMain.push("  static void Parse(ByteSource&, type&);");
    header.internalMain.push(
      "  static soia::reflection::Type GetType(soia_type<type>);",
    );
    header.internalMain.push("  static void RegisterRecords(");
    header.internalMain.push("      soia_type<type>,");
    header.internalMain.push("      soia::reflection::RecordRegistry&);");
    header.internalMain.push(
      `  static constexpr bool IsStruct() { return ${
        recordType === "struct"
      }; }`,
    );
    header.internalMain.push(
      `  static constexpr bool IsEnum() { return ${recordType === "enum"}; }`,
    );
    header.internalMain.push("};");
    header.internalMain.push("");
    header.internal.push(
      `inline ::soia_internal::${this.namespace}::${adapterName} GetAdapter(`,
    );
    header.internal.push(`    ::soia_internal::soia_type<${qualifiedName}>);`);
    header.internal.push("");
  }

  private writeCodeForConstantField(field: EnumField): void {
    if (!this.addSoiagenSymbol(field.structType)) return;
    const { soiagen } = this.header;
    soiagen.push(`#ifndef SOIAGEN_${field.structType}`);
    soiagen.push(`#define SOIAGEN_${field.structType}`);
    soiagen.push(`struct ${field.structType} {`);
    soiagen.push(
      `  static constexpr absl::string_view kFieldName = "${field.fieldName}";`,
    );
    soiagen.push("};");
    soiagen.push("");
    soiagen.push(`constexpr auto ${field.identifier} = ${field.structType}();`);
    soiagen.push("#endif");
    soiagen.push("");
  }

  private writeCodeForValueField(field: EnumField): void {
    const { fieldName, structType } = field;
    if (!this.addSoiagenSymbol(structType)) return;
    const optionType = `${fieldName}_option`;
    {
      const { soiagen } = this.header;
      soiagen.push(`#ifndef SOIAGEN_${structType}`);
      soiagen.push(`#define SOIAGEN_${structType}`);
      soiagen.push("template <typename T>");
      soiagen.push(`struct ${structType};`);
      soiagen.push("");
      soiagen.push("namespace reflection {");
      soiagen.push(`struct ${optionType} {`);
      soiagen.push(
        `  static constexpr absl::string_view kFieldName = "${fieldName}";`,
      );
      soiagen.push("");
      soiagen.push("  template <typename T>");
      soiagen.push(`  static ${structType}<T> wrap(T input) {`);
      soiagen.push(`    return ${structType}(std::move(input));`);
      soiagen.push("  }");
      soiagen.push("");
      soiagen.push("  template <typename Enum>");
      soiagen.push("  static auto* get_or_null(Enum& e) {");
      soiagen.push(
        `    return e.is_${fieldName}() ? &e.as_${fieldName}() : nullptr;`,
      );
      soiagen.push("  }");
      soiagen.push("};");
      soiagen.push("}  // namespace reflection");
      soiagen.push("");
      soiagen.push("template <typename T>");
      soiagen.push(`struct ${structType} {`);
      soiagen.push("  using value_type = T;");
      soiagen.push(
        `  using option_type = ::soiagen::reflection::${optionType};`,
      );
      soiagen.push("");
      soiagen.push("  T value{};");
      soiagen.push("");
      soiagen.push(`  ${structType}() = default;`);
      soiagen.push(
        `  explicit ${structType}(T value): value(std::move(value)) {}`,
      );
      soiagen.push("};");
      soiagen.push("#endif");
      soiagen.push("");
    }
    {
      const { soiagen } = this.testingHeader;
      soiagen.push(`#ifndef TESTING_SOIAGEN_${structType}`);
      soiagen.push(`#define TESTING_SOIAGEN_${structType}`);
      soiagen.push("template <typename ValueMatcher = decltype(_)>");
      const functionName =
        "Is" + convertCase(fieldName, "lower_underscore", "UpperCamel");
      soiagen.push(`auto ${functionName}(ValueMatcher matcher = _) {`);
      soiagen.push("  using ::testing::soia_internal::EnumValueIsMatcher;");
      soiagen.push(`  using Option = ::soiagen::reflection::${optionType};`);
      soiagen.push(
        "  return EnumValueIsMatcher<Option, ValueMatcher>(std::move(matcher));",
      );
      soiagen.push("}");
      soiagen.push("#endif");
      soiagen.push("");
    }
  }

  private writeCodeForMethod(method: Method): void {
    const { typeSpeller } = this;
    const { mainMiddle } = this.header;
    const methodName = method.name.text;
    const requestType = typeSpeller.getCcType(method.requestType!);
    const responseType = typeSpeller.getCcType(method.responseType!);
    mainMiddle.push(`struct ${methodName} {`);
    mainMiddle.push(`  using request_type = ${requestType};`);
    mainMiddle.push(`  using response_type = ${responseType};`);
    mainMiddle.push(
      `  static constexpr absl::string_view kMethodName = "${methodName}";`,
    );
    mainMiddle.push(`  static constexpr int kNumber = ${method.number};`);
    mainMiddle.push("};");
    mainMiddle.push("");
  }

  private writeCodeForConstant(constant: Constant): void {
    const { header, source, typeSpeller } = this;
    const name = `k_${constant.name.text.toLowerCase()}`;
    const type = typeSpeller.getCcType(constant.type!);
    const ccStringLiteral = JSON.stringify(
      JSON.stringify(constant.valueAsDenseJson),
    );
    header.mainMiddle.push(`const ${type}& ${name}();`);
    header.mainMiddle.push("");
    source.mainMiddle.push(`const ${type}& ${name}() {`);
    source.mainMiddle.push(`  static auto* result = new ${type}(`);
    source.mainMiddle.push(`      ::soia::Parse<${type}>(`);
    source.mainMiddle.push(`          ${ccStringLiteral})`);
    source.mainMiddle.push("          .value());");
    source.mainMiddle.push("  return *result;");
    source.mainMiddle.push("}");
    source.mainMiddle.push("");
  }

  private writeIncludes(): void {
    const { header, source, testingHeader } = this;
    {
      const headerPath =
        "soiagen/" + this.inModule.path.replace(/\.soia$/, ".h");
      source.includes.push(`#include "${headerPath}"`);
      testingHeader.includes.push(`#include "${headerPath}"`);
    }
    for (const h of [...this.includes].sort()) {
      header.includes.push(`#include ${h}`);
      testingHeader.includes.push(
        `#include ${h.replace(/\.h\"$/, '.testing.h"')}`,
      );
    }
  }

  private addSoiagenSymbol(symbol: string): boolean {
    if (this.seenSoiagenSymbols.has(symbol)) return false;
    this.seenSoiagenSymbols.add(symbol);
    return true;
  }

  private readonly includes = new Set<string>();
  private readonly typeSpeller: TypeSpeller;
  private readonly recursivityResolver: RecursvityResolver;
  private readonly namespace: string;
  private readonly seenSoiagenSymbols = new Set<string>();

  readonly header: FileContents = new FileContents(".h");
  readonly source: FileContents = new FileContents(".cc");
  readonly testingHeader: FileContents = new FileContents(".testing.h");
}

class FileContents {
  constructor(readonly extension: ".h" | ".cc" | ".testing.h") {}

  namespace: string = "";

  readonly includes: string[] = [];
  /** Group within the ::soiagen namespace. */
  readonly soiagen: string[] = [];
  /** First group within the ::soiagen_my_module namespace. */
  readonly mainTop: string[] = [];
  /** Second group within the ::soiagen_my_module namespace. */
  readonly mainMiddle: string[] = [];
  /** Third group within the ::soiagen_my_module namespace. */
  readonly mainBottom: string[] = [];
  /** Group within the anonymous namespace. Only in the .cc. */
  readonly anonymous: string[] = [];
  /** Group within the ::soia_internal namespace. */
  readonly internal: string[] = [];
  /**
   * First group within the ::soia_internal_my_module namespace. Only in the
   * .h.
   */
  readonly internalMainTop: string[] = [];
  /** Group within the ::soia_internal::my::module namespace. */
  readonly internalMain: string[] = [];
}

function fileContentsToCode(fileContents: FileContents): string {
  const { extension, namespace } = fileContents;
  const lines = [
    "//  ______                        _               _  _  _",
    "//  |  _  \\                      | |             | |(_)| |",
    "//  | | | |  ___    _ __    ___  | |_    ___   __| | _ | |_",
    "//  | | | | / _ \\  | '_ \\  / _ \\ | __|  / _ \\ / _` || || __|",
    "//  | |/ / | (_) | | | | || (_) || |_  |  __/| (_| || || |_ ",
    "//  |___/   \\___/  |_| |_| \\___/  \\__|  \\___| \\__,_||_| \\__|",
    "",
  ];
  if (extension === ".h" || extension === ".testing.h") {
    const includeGuard =
      `${namespace}${extension.replace(/\./g, "_")}`.toUpperCase();
    lines.push(`#ifndef ${includeGuard}`);
    lines.push(`#define ${includeGuard}`);
    lines.push("");
  }
  fileContents.includes.forEach((l) => lines.push(l));
  lines.push("");
  if (extension === ".h") {
    lines.push("namespace soia_internal {");
    lines.push(`namespace ${namespace} {`);
    fileContents.internalMainTop.forEach((l) => lines.push(l));
    lines.push(`}  // namespace ${namespace}`);
    lines.push("}  // namespace soia_internal");
    lines.push("");
  } else if (extension === ".cc") {
    lines.push("namespace {");
    lines.push("");
    fileContents.anonymous.forEach((l) => lines.push(l));
    lines.push("");
    lines.push("}  // namespace");
    lines.push("");
  }
  if (extension === ".testing.h") {
    lines.push("namespace testing {");
  }
  lines.push("namespace soiagen {");
  fileContents.soiagen.forEach((l) => lines.push(l));
  lines.push("}  // namespace soiagen");
  if (extension === ".h" || extension === ".cc") {
    lines.push("");
    lines.push(`namespace ${namespace} {`);
    fileContents.mainTop.forEach((l) => lines.push(l));
    lines.push("");
    fileContents.mainMiddle.forEach((l) => lines.push(l));
    lines.push("");
    fileContents.mainBottom.forEach((l) => lines.push(l));
    lines.push("");
    lines.push(`}  // namespace ${namespace}`);
    lines.push("namespace soia_internal {");
    lines.push(`namespace ${namespace} {`);
    lines.push("");
    fileContents.internalMain.forEach((l) => lines.push(l));
    lines.push("");
    lines.push(`}  // namespace ${namespace}`);
    lines.push("");
    fileContents.internal.forEach((l) => lines.push(l));
    lines.push("");
    lines.push("}  // namespace soia_internal");
    lines.push("");
  } else {
    lines.push("}  // namespace testing");
    lines.push("");
  }
  if (extension === ".h" || extension === ".testing.h") {
    lines.push("#endif");
  }
  return (
    lines
      .map((l) => `${l}\n`)
      .join("")
      // Remove empty line following "public" or "private".
      .replace(/((public:|private:)\n)\n+/g, "$1")
      // Remove empty line preceding a closed curly bracket.
      .replace(/\n(\n *\})/g, "$1")
      // Coalesce consecutive empty lines.
      .replace(/\n\n\n+/g, "\n\n")
      .replace(/\n\n$/g, "\n")
  );
}

export const GENERATOR = new CcCodeGenerator();

function maybeEscapeLowerCaseName(name: string) {
  return CC_KEYWORDS.has(name) ? `${name}_` : name;
}

function numberToCharLiterals(n: number): string {
  const decimal = `${n}`;
  let result = "";
  for (let i = 0; i < decimal.length; ++i) {
    if (i !== 0) {
      result += ", ";
    }
    result += `'${decimal[i]}'`;
  }
  return result;
}

function bytesToIntLiterals(bytes: readonly number[]): string {
  return bytes.map((b) => `${b}`).join(", ");
}
