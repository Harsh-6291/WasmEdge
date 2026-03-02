// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2025 Second State INC

#pragma once

#include "common/errcode.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace WasmEdge {
namespace AST {
namespace Component {

namespace ComponentNameParser {
inline bool isKebabString(std::string_view Input) {
  bool IsFirstPart = true;
  bool Uppercase = false;
  bool Lowercase = false;
  bool Digit = false;

  for (char C : Input) {
    if (islower(C)) {
      if (Uppercase)
        return false;
      Lowercase = true;
    } else if (isupper(C)) {
      if (Lowercase)
        return false;
      Uppercase = true;
    } else if (isdigit(C)) {
      if (IsFirstPart && !(Uppercase || Lowercase))
        return false;
      Digit = true;
    } else if (C == '-') {
      if (Uppercase || Lowercase || Digit) {
        IsFirstPart = false;
        Uppercase = false;
        Lowercase = false;
        Digit = false;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }

  return Input.size() > 0 && Input.back() != '-';
}

inline bool isLowercaseKebabString(std::string_view Input) {
  return isKebabString(Input) &&
         std::all_of(Input.begin(), Input.end(), [](char c) {
           return c == '-' || islower(c) || isdigit(c);
         });
}

inline bool isEOF(std::string_view Input) { return Input.empty(); }

inline bool readUntil(std::string_view &Input, char delim,
                      std::string_view &output) {
  size_t Pos = Input.find(delim);
  if (Pos == Input.npos) {
    return false;
  }

  output = Input.substr(0, Pos);
  Input.remove_prefix(Pos + 1);
  return true;
}

inline bool tryRead(std::string_view Prefix, std::string_view &Name) {
  if (Prefix.size() > Name.size())
    return false;
  if (Prefix != Name.substr(0, Prefix.size()))
    return false;

  Name.remove_prefix(Prefix.size());
  return true;
}

inline bool tryReadKebab(std::string_view &Input, std::string_view &Output) {
  size_t Pos = 0;
  while (Pos < Input.size()) {
    if (isalnum(Input[Pos]) || Input[Pos] == '-') {
      Pos++;
    } else {
      break;
    }
  }
  Output = Input.substr(0, Pos);
  Input.remove_prefix(Pos);
  return isKebabString(Output);
}
} // namespace ComponentNameParser

enum class ComponentNameKind {
  Invalid,
  Constructor,
  Method,
  Static,
  InterfaceType,
  Label,
  Plain,  // Legacy mapping
  Scoped, // Legacy mapping
  Hash    // Legacy mapping
};

struct ComponentName {
  std::string_view OriName;
  std::string_view NoTagName;
  ComponentNameKind Kind;
  union Details {
    Details() {}
    struct {
      std::string_view Label;
    } Constructor;
    struct {
      std::string_view Resource;
      std::string_view Method;
    } Method, Static;
    struct {
      std::string_view Namespace;
      std::string_view Package;
      std::string_view Interface;
      std::string_view Projection;
      std::string_view Version;
    } Interface;
  } Detail;

  // Additional fields for compatibility during transition
  std::string NameStr;
  std::string HashStr;

  ComponentName() = default;
  ComponentName(std::string_view N)
      : OriName(N), Kind(ComponentNameKind::Invalid), Detail({}) {
    parse();
  }

  void parse() {
    using namespace ComponentNameParser;
    using namespace std::literals;

    // First do the legacy / simple parsing for our loader integration
    // constraints
    NameStr = std::string(OriName);
    HashStr.clear();

    // We run the full parser
    auto Next = OriName;
    Kind = ComponentNameKind::Invalid;
    Detail = {};

    if (tryRead("[constructor]"sv, Next)) {
      if (!isKebabString(Next)) {
        return;
      }
      Detail.Constructor.Label = Next;
      NoTagName = Next;
      Kind = ComponentNameKind::Constructor;
      return;
    }

    auto tryReadResourceWithLabel = [&](std::string_view Tag,
                                        std::string_view &Resource,
                                        std::string_view &Label) -> bool {
      if (!tryRead(Tag, Next)) {
        return false;
      }
      auto TmpNoTagName = Next;
      if (!readUntil(Next, '.', Resource)) {
        return false;
      }
      if (!isKebabString(Resource) || !isKebabString(Next)) {
        return false;
      }
      NoTagName = TmpNoTagName;
      Label = Next;
      return true;
    };

    if (tryReadResourceWithLabel("[method]"sv, Detail.Method.Resource,
                                 Detail.Method.Method)) {
      Kind = ComponentNameKind::Method;
      return;
    }

    if (tryReadResourceWithLabel("[static]"sv, Detail.Static.Resource,
                                 Detail.Static.Method)) {
      Kind = ComponentNameKind::Static;
      return;
    }

    if (tryRead("[async]"sv, Next)) {
      NoTagName = Next;
      return;
    }
    if (tryRead("[async method]"sv, Next)) {
      NoTagName = Next;
      return;
    }
    if (tryRead("[async static]"sv, Next)) {
      NoTagName = Next;
      return;
    }

    if (Next.size() != 0 && Next[0] == '[') {
      return;
    }
    NoTagName = Next;

    if (tryRead("unlocked-dep="sv, Next)) {
      return;
    }
    if (tryRead("locked-dep="sv, Next)) {
      return;
    }
    if (tryRead("url="sv, Next)) {
      return;
    }

    // Legacy Hash Check integration
    if (auto ColonPos = OriName.find(':');
        ColonPos != std::string::npos &&
        OriName.compare(0, 10, "integrity-") == 0) {
      Kind = ComponentNameKind::Hash;
      HashStr = OriName.substr(ColonPos + 1);
      NameStr = OriName.substr(0, ColonPos);
      return;
    }

    if (tryRead("integrity="sv, Next)) {
      return;
    }

    if (Next.find(':') != Next.npos) {
      std::string_view Namespace, Package, Interface, Projection, Version;
      int Counter = 0;
      while (readUntil(Next, ':', Namespace)) {
        Counter++;
        if (!isLowercaseKebabString(Namespace)) {
          return;
        }
      }
      if (Counter == 0 || Counter != 1) {
        return;
      }

      if (!tryReadKebab(Next, Package)) {
        return;
      }

      Counter = 0;
      while (!isEOF(Next) && Next[0] == '/') {
        Next.remove_prefix(1);
        Counter++;
        if (!tryReadKebab(Next, Interface)) {
          return;
        }
      }

      if (Counter == 0 || Counter != 1) {
        return;
      }

      if (!isEOF(Next) && Next[0] == '@') {
        Next.remove_prefix(1);
        Version = Next;
      }

      Detail.Interface.Namespace = Namespace;
      Detail.Interface.Package = Package;
      Detail.Interface.Interface = Interface;
      Detail.Interface.Projection = Projection;
      Detail.Interface.Version = Version;
      Kind = ComponentNameKind::InterfaceType;
    } else {
      if (!isKebabString(Next)) {
        return;
      }
      Kind = ComponentNameKind::Label;
    }
  }

  std::string_view getOriginalName() const { return OriName; }
  ComponentNameKind getKind() const { return Kind; }

  std::string getFullName() const { return std::string(OriName); }

  bool operator==(std::string_view V) const { return OriName == V; }
};

} // namespace Component
} // namespace AST
} // namespace WasmEdge
