//////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2017 EMC Corporation
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is EMC Corporation
///
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include "catch.hpp"
#include "StorageEngineMock.h"

#include "IResearch/IResearchFilterFactory.h"
#include "IResearch/IResearchLinkMeta.h"
#include "IResearch/IResearchViewMeta.h"

#include "StorageEngine/EngineSelectorFeature.h"
#include "RestServer/AqlFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "RestServer/TraverserEngineRegistryFeature.h"
#include "Aql/Ast.h"
#include "Aql/Query.h"

#include "analysis/analyzers.hpp"
#include "analysis/token_streams.hpp"
#include "analysis/token_attributes.hpp"
#include "search/term_filter.hpp"
#include "search/prefix_filter.hpp"
#include "search/range_filter.hpp"
#include "search/granular_range_filter.hpp"
#include "search/boolean_filter.hpp"
#include "search/phrase_filter.hpp"

namespace {

std::string mangleName(irs::string_ref const& name, irs::string_ref const& suffix) {
  std::string mangledName(name.c_str(), name.size());
  mangledName += '\0';
  mangledName.append(suffix.c_str(), suffix.size());
  return mangledName;
}

std::string mangleBool(irs::string_ref const& name) { return mangleName(name, "_b"); }
std::string mangleNull(irs::string_ref const& name) { return mangleName(name, "_n"); }
std::string mangleNumeric(irs::string_ref const& name) { return mangleName(name, "_d"); }

void assertFilterSuccess(std::string const& queryString, irs::filter const& expected) {
  TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");

  std::shared_ptr<arangodb::velocypack::Builder> bindVars;
  auto options = std::make_shared<arangodb::velocypack::Builder>();

  arangodb::aql::Query query(
     false, &vocbase, arangodb::aql::QueryString(queryString),
     bindVars, options,
     arangodb::aql::PART_MAIN
  );

  auto const parseResult = query.parse();
  REQUIRE(TRI_ERROR_NO_ERROR == parseResult.code);

  auto* root = query.ast()->root();
  REQUIRE(root);
  auto* filterNode = root->getMember(1);
  REQUIRE(filterNode);

  irs::Or actual;
  CHECK((arangodb::iresearch::FilterFactory::filter(nullptr, *filterNode)));
  CHECK((arangodb::iresearch::FilterFactory::filter(&actual, *filterNode)));
  CHECK(expected == actual);
}

void assertFilterFail(std::string const& queryString) {
  TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");

  arangodb::aql::Query query(
     false, &vocbase, arangodb::aql::QueryString(queryString),
     nullptr, nullptr,
     arangodb::aql::PART_MAIN
  );

  auto const parseResult = query.parse();
  REQUIRE(TRI_ERROR_NO_ERROR == parseResult.code);

  auto* root = query.ast()->root();
  REQUIRE(root);
  auto* filterNode = root->getMember(1);
  REQUIRE(filterNode);

  irs::Or actual;
  CHECK((!arangodb::iresearch::FilterFactory::filter(nullptr, *filterNode)));
  CHECK((!arangodb::iresearch::FilterFactory::filter(&actual, *filterNode)));
}

}

// -----------------------------------------------------------------------------
// --SECTION--                                                 setup / tear-down
// -----------------------------------------------------------------------------

struct IResearchFilterSetup {
  StorageEngineMock engine;
  arangodb::application_features::ApplicationServer server;

  IResearchFilterSetup(): server(nullptr, nullptr) {
    arangodb::EngineSelectorFeature::ENGINE = &engine;
    arangodb::application_features::ApplicationFeature* feature;

    // AqlFeature
    arangodb::application_features::ApplicationServer::server->addFeature(
      feature = new arangodb::AqlFeature(&server)
    );
    feature->start();
    feature->prepare();

    // QueryRegistryFeature
    arangodb::application_features::ApplicationServer::server->addFeature(
      feature = new arangodb::QueryRegistryFeature(&server)
    );
    feature->start();
    feature->prepare();

    // TraverserEngineRegistryFeature (required for AqlFeature::stop() to work)
    arangodb::application_features::ApplicationServer::server->addFeature(
      feature = new arangodb::TraverserEngineRegistryFeature(&server)
    );
    feature->start();
    feature->prepare();
  }

  ~IResearchFilterSetup() {
    arangodb::AqlFeature(&server).stop(); // unset singleton instance
    arangodb::application_features::ApplicationServer::server = nullptr;
    arangodb::EngineSelectorFeature::ENGINE = nullptr;
  }
}; // IResearchFilterSetup

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief setup
////////////////////////////////////////////////////////////////////////////////

TEST_CASE("IResearchFilterTest", "[iresearch][iresearch-filter]") {
  IResearchFilterSetup s;
  UNUSED(s);

SECTION("BinaryIn") {
  // simple attribute
  {
    std::string const queryString = "FOR d IN collection FILTER d.a in ['1','2','3'] RETURN d";

    irs::Or expected;
    auto& root = expected.add<irs::Or>();
    root.add<irs::by_term>().field("a").term("1");
    root.add<irs::by_term>().field("a").term("2");
    root.add<irs::by_term>().field("a").term("3");

    assertFilterSuccess(queryString, expected);
  }

  // complex attribute name
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.e.f in ['1','2','3'] RETURN d";

    irs::Or expected;
    auto& root = expected.add<irs::Or>();
    root.add<irs::by_term>().field("a.b.c.e.f").term("1");
    root.add<irs::by_term>().field("a.b.c.e.f").term("2");
    root.add<irs::by_term>().field("a.b.c.e.f").term("3");

    assertFilterSuccess(queryString, expected);
  }

  // heterogeneous array values
  {
    std::string const queryString = "FOR d IN collection FILTER d.quick.brown.fox in ['1',null,true,false,2] RETURN d";

    irs::Or expected;
    auto& root = expected.add<irs::Or>();
    root.add<irs::by_term>().field("quick.brown.fox").term("1");
    root.add<irs::by_term>().field(mangleNull("quick.brown.fox")).term(irs::null_token_stream::value_null());
    root.add<irs::by_term>().field(mangleBool("quick.brown.fox")).term(irs::boolean_token_stream::value_true());
    root.add<irs::by_term>().field(mangleBool("quick.brown.fox")).term(irs::boolean_token_stream::value_false());
    {
      irs::numeric_token_stream stream;
      auto& term = stream.attributes().get<irs::term_attribute>();
      stream.reset(2.);
      CHECK(stream.next());
      root.add<irs::by_term>().field(mangleNumeric("quick.brown.fox")).term(term->value());
    }

    assertFilterSuccess(queryString, expected);
  }

  // empty array
  {
    std::string const queryString = "FOR d IN collection FILTER d.quick.brown.fox in [] RETURN d";

    irs::Or expected;
    auto& root = expected.add<irs::empty>();

    assertFilterSuccess(queryString, expected);
  }

  // invalid attribute access
  assertFilterFail("FOR d IN VIEW myView FILTER 'd.a' in [1,2,3] RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER null in [1,2,3] RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER true in [1,2,3] RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER false in [1,2,3] RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER 4 in [1,2,3] RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER 4.5 in [1,2,3] RETURN d");

  // not a value in array
  assertFilterFail("FOR d IN collection FILTER d.a in ['1',['2'],'3'] RETURN d");
  // not a constant in array
  assertFilterFail("FOR d IN collection FILTER d.a in ['1', d, '3'] RETURN d");

  // numeric range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.e.f in 4..5 RETURN d";

    irs::numeric_token_stream minTerm; minTerm.reset(4.0);
    irs::numeric_token_stream maxTerm; maxTerm.reset(5.0);

    irs::Or expected;
    auto& range = expected.add<irs::by_granular_range>();
    range.field(mangleNumeric("a.b.c.e.f"));
    range.include<irs::Bound::MIN>(true).insert<irs::Bound::MIN>(minTerm);
    range.include<irs::Bound::MAX>(true).insert<irs::Bound::MAX>(maxTerm);

    assertFilterSuccess(queryString, expected);
  }

  // numeric floating range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.e.f in 4.5..5.0 RETURN d";

    irs::numeric_token_stream minTerm; minTerm.reset(4.5);
    irs::numeric_token_stream maxTerm; maxTerm.reset(5.0);

    irs::Or expected;
    auto& range = expected.add<irs::by_granular_range>();
    range.field(mangleNumeric("a.b.c.e.f"));
    range.include<irs::Bound::MIN>(true).insert<irs::Bound::MIN>(minTerm);
    range.include<irs::Bound::MAX>(true).insert<irs::Bound::MAX>(maxTerm);

    assertFilterSuccess(queryString, expected);
  }

  // numeric int-float range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.e.f in 4..5.0 RETURN d";

    irs::numeric_token_stream minTerm; minTerm.reset(4.0);
    irs::numeric_token_stream maxTerm; maxTerm.reset(5.0);

    irs::Or expected;
    auto& range = expected.add<irs::by_granular_range>();
    range.field(mangleNumeric("a.b.c.e.f"));
    range.include<irs::Bound::MIN>(true).insert<irs::Bound::MIN>(minTerm);
    range.include<irs::Bound::MAX>(true).insert<irs::Bound::MAX>(maxTerm);

    assertFilterSuccess(queryString, expected);
  }

  // string range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.e.f in '4'..'5' RETURN d";

    irs::Or expected;
    auto& range = expected.add<irs::by_range>();
    range.field("a.b.c.e.f");
    range.include<irs::Bound::MIN>(true).term<irs::Bound::MIN>("4");
    range.include<irs::Bound::MAX>(true).term<irs::Bound::MAX>("5");

    assertFilterSuccess(queryString, expected);
  }

  // boolean range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.e.f in false..true RETURN d";

    irs::Or expected;
    auto& range = expected.add<irs::by_range>();
    range.field(mangleBool("a.b.c.e.f"));
    range.include<irs::Bound::MIN>(true).term<irs::Bound::MIN>(irs::boolean_token_stream::value_false());
    range.include<irs::Bound::MAX>(true).term<irs::Bound::MAX>(irs::boolean_token_stream::value_true());

    assertFilterSuccess(queryString, expected);
  }

  // null range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.e.f in null..null RETURN d";

    irs::Or expected;
    auto& range = expected.add<irs::by_range>();
    range.field(mangleNull("a.b.c.e.f"));
    range.include<irs::Bound::MIN>(true).term<irs::Bound::MIN>(irs::null_token_stream::value_null());
    range.include<irs::Bound::MAX>(true).term<irs::Bound::MAX>(irs::null_token_stream::value_null());

    assertFilterSuccess(queryString, expected);
  }

  // invalid attribute access
  assertFilterFail("FOR d IN VIEW myView FILTER 'd.a' in 4..5 RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER 4 in 4..5 RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER 4.3 in 4..5 RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER null in 4..5 RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER true in 4..5 RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER false in 4..5 RETURN d");

  // invalid heterogeneous ranges
  assertFilterFail("FOR d IN VIEW myView FILTER d.a in 'a'..4 RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER d.a in 1..null RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER d.a in false..5.5 RETURN d");

  // invalid range (supported by AQL)
  assertFilterFail("FOR d IN VIEW myView FILTER d.a in 1..4..5 RETURN d");
}

SECTION("BinaryNotIn") {
  // simple attribute
  {
    std::string const queryString = "FOR d IN collection FILTER d.a not in ['1','2','3'] RETURN d";

    irs::Or expected;
    auto& root = expected.add<irs::Not>().filter<irs::And>();
    root.add<irs::by_term>().field("a").term("1");
    root.add<irs::by_term>().field("a").term("2");
    root.add<irs::by_term>().field("a").term("3");

    assertFilterSuccess(queryString, expected);
  }

  // complex attribute name
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.e.f not in ['1','2','3'] RETURN d";

    irs::Or expected;
    auto& root = expected.add<irs::Not>().filter<irs::And>();
    root.add<irs::by_term>().field("a.b.c.e.f").term("1");
    root.add<irs::by_term>().field("a.b.c.e.f").term("2");
    root.add<irs::by_term>().field("a.b.c.e.f").term("3");

    assertFilterSuccess(queryString, expected);
  }

  // heterogeneous array values
  {
    std::string const queryString = "FOR d IN collection FILTER d.quick.brown.fox not in ['1',null,true,false,2] RETURN d";

    irs::Or expected;
    auto& root = expected.add<irs::Not>().filter<irs::And>();
    root.add<irs::by_term>().field("quick.brown.fox").term("1");
    root.add<irs::by_term>().field(mangleNull("quick.brown.fox")).term(irs::null_token_stream::value_null());
    root.add<irs::by_term>().field(mangleBool("quick.brown.fox")).term(irs::boolean_token_stream::value_true());
    root.add<irs::by_term>().field(mangleBool("quick.brown.fox")).term(irs::boolean_token_stream::value_false());
    {
      irs::numeric_token_stream stream;
      auto& term = stream.attributes().get<irs::term_attribute>();
      stream.reset(2.);
      CHECK(stream.next());
      root.add<irs::by_term>().field(mangleNumeric("quick.brown.fox")).term(term->value());
    }

    assertFilterSuccess(queryString, expected);
  }

  // empty array
  {
    std::string const queryString = "FOR d IN collection FILTER d.quick.brown.fox not in [] RETURN d";

    irs::Or expected;
    auto& root = expected.add<irs::all>();

    assertFilterSuccess(queryString, expected);
  }

  // invalid attribute access
  assertFilterFail("FOR d IN VIEW myView FILTER 'd.a' not in [1,2,3] RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER null not in [1,2,3] RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER true not in [1,2,3] RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER false not in [1,2,3] RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER 4 not in [1,2,3] RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER 4.5 not in [1,2,3] RETURN d");

  // not a value in array
  assertFilterFail("FOR d IN collection FILTER d.a not in ['1',['2'],'3'] RETURN d");

  // not a constant in array
  assertFilterFail("FOR d IN collection FILTER d.a not in ['1', d, '3'] RETURN d");

  // numeric range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.e.f not in 4..5 RETURN d";

    irs::numeric_token_stream minTerm; minTerm.reset(4.0);
    irs::numeric_token_stream maxTerm; maxTerm.reset(5.0);

    irs::Or expected;
    auto& range = expected.add<irs::Not>().filter<irs::Or>().add<irs::by_granular_range>();
    range.field(mangleNumeric("a.b.c.e.f"));
    range.include<irs::Bound::MIN>(true).insert<irs::Bound::MIN>(minTerm);
    range.include<irs::Bound::MAX>(true).insert<irs::Bound::MAX>(maxTerm);

    assertFilterSuccess(queryString, expected);
  }

  // numeric floating range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.e.f not in 4.5..5.0 RETURN d";

    irs::numeric_token_stream minTerm; minTerm.reset(4.5);
    irs::numeric_token_stream maxTerm; maxTerm.reset(5.0);

    irs::Or expected;
    auto& range = expected.add<irs::Not>().filter<irs::Or>().add<irs::by_granular_range>();
    range.field(mangleNumeric("a.b.c.e.f"));
    range.include<irs::Bound::MIN>(true).insert<irs::Bound::MIN>(minTerm);
    range.include<irs::Bound::MAX>(true).insert<irs::Bound::MAX>(maxTerm);

    assertFilterSuccess(queryString, expected);
  }

  // numeric int-float range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.e.f not in 4..5.0 RETURN d";

    irs::numeric_token_stream minTerm; minTerm.reset(4.0);
    irs::numeric_token_stream maxTerm; maxTerm.reset(5.0);

    irs::Or expected;
    auto& range = expected.add<irs::Not>().filter<irs::Or>().add<irs::by_granular_range>();
    range.field(mangleNumeric("a.b.c.e.f"));
    range.include<irs::Bound::MIN>(true).insert<irs::Bound::MIN>(minTerm);
    range.include<irs::Bound::MAX>(true).insert<irs::Bound::MAX>(maxTerm);

    assertFilterSuccess(queryString, expected);
  }

  // string range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.e.f not in '4'..'5' RETURN d";

    irs::Or expected;
    auto& range = expected.add<irs::Not>().filter<irs::Or>().add<irs::by_range>();
    range.field("a.b.c.e.f");
    range.include<irs::Bound::MIN>(true).term<irs::Bound::MIN>("4");
    range.include<irs::Bound::MAX>(true).term<irs::Bound::MAX>("5");

    assertFilterSuccess(queryString, expected);
  }

  // boolean range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.e.f not in false..true RETURN d";

    irs::Or expected;
    auto& range = expected.add<irs::Not>().filter<irs::Or>().add<irs::by_range>();
    range.field(mangleBool("a.b.c.e.f"));
    range.include<irs::Bound::MIN>(true).term<irs::Bound::MIN>(irs::boolean_token_stream::value_false());
    range.include<irs::Bound::MAX>(true).term<irs::Bound::MAX>(irs::boolean_token_stream::value_true());

    assertFilterSuccess(queryString, expected);
  }

  // null range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.e.f not in null..null RETURN d";

    irs::Or expected;
    auto& range = expected.add<irs::Not>().filter<irs::Or>().add<irs::by_range>();
    range.field(mangleNull("a.b.c.e.f"));
    range.include<irs::Bound::MIN>(true).term<irs::Bound::MIN>(irs::null_token_stream::value_null());
    range.include<irs::Bound::MAX>(true).term<irs::Bound::MAX>(irs::null_token_stream::value_null());

    assertFilterSuccess(queryString, expected);
  }

  // invalid attribute access
  assertFilterFail("FOR d IN VIEW myView FILTER 'd.a' not in 4..5 RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER 4 not in 4..5 RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER 4.3 not in 4..5 RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER null not in 4..5 RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER true not in 4..5 RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER false not in 4..5 RETURN d");

  // not invalid heterogeneous ranges
  assertFilterFail("FOR d IN VIEW myView FILTER d.a not in 'a'..4 RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER d.a not in 1..null RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER d.a not in false..5.5 RETURN d");

  // invalid range (supported by AQL)
  assertFilterFail("FOR d IN VIEW myView FILTER d.a not in 1..4..5 RETURN d");
}

SECTION("BinaryEq") {
  // simple string attribute
  {
    std::string const queryString = "FOR d IN collection FILTER d.a == '1' RETURN d";

    irs::Or expected;
    expected.add<irs::by_term>().field("a").term("1");

    assertFilterSuccess(queryString, expected);
  }

  // complex attribute name, string
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c == '1' RETURN d";

    irs::Or expected;
    expected.add<irs::by_term>().field("a.b.c").term("1");

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, true
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c == true RETURN d";

    irs::Or expected;
    expected.add<irs::by_term>().field(mangleBool("a.b.c")).term(irs::boolean_token_stream::value_true());

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, false
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.bool == false RETURN d";

    irs::Or expected;
    expected.add<irs::by_term>().field(mangleBool("a.b.c.bool")).term(irs::boolean_token_stream::value_false());

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, null
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.bool == null RETURN d";

    irs::Or expected;
    expected.add<irs::by_term>().field(mangleNull("a.b.c.bool")).term(irs::null_token_stream::value_null());

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, numeric
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.numeric == 3 RETURN d";

    irs::numeric_token_stream stream;
    stream.reset(3.);
    CHECK(stream.next());
    auto& term = stream.attributes().get<irs::term_attribute>();

    irs::Or expected;
    expected.add<irs::by_term>().field(mangleNumeric("a.b.c.numeric")).term(term->value());

    assertFilterSuccess(queryString, expected);
  }

  // invalid attribute access
  assertFilterFail("FOR d IN collection FILTER d == '1' RETURN d");
}

SECTION("BinaryNotEq") {
  // simple string attribute
  {
    std::string const queryString = "FOR d IN collection FILTER d.a != '1' RETURN d";

    irs::Or expected;
    expected.add<irs::Not>().filter<irs::by_term>().field("a").term("1");

    assertFilterSuccess(queryString, expected);
  }

  // complex attribute name, string
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c != '1' RETURN d";

    irs::Or expected;
    expected.add<irs::Not>().filter<irs::by_term>().field("a.b.c").term("1");

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, true
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c != true RETURN d";

    irs::Or expected;
    expected.add<irs::Not>().filter<irs::by_term>().field(mangleBool("a.b.c")).term(irs::boolean_token_stream::value_true());

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, false
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.bool != false RETURN d";

    irs::Or expected;
    expected.add<irs::Not>().filter<irs::by_term>().field(mangleBool("a.b.c.bool")).term(irs::boolean_token_stream::value_false());

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, null
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.bool != null RETURN d";

    irs::Or expected;
    expected.add<irs::Not>().filter<irs::by_term>().field(mangleNull("a.b.c.bool")).term(irs::null_token_stream::value_null());

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, numeric
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.numeric != 3 RETURN d";

    irs::numeric_token_stream stream;
    stream.reset(3.);
    CHECK(stream.next());
    auto& term = stream.attributes().get<irs::term_attribute>();

    irs::Or expected;
    expected.add<irs::Not>().filter<irs::by_term>().field(mangleNumeric("a.b.c.numeric")).term(term->value());

    assertFilterSuccess(queryString, expected);
  }

  // invalid attribute access
  assertFilterFail("FOR d IN collection FILTER d != '1' RETURN d");
}

SECTION("BinaryGE") {
  // simple string attribute
  {
    std::string const queryString = "FOR d IN collection FILTER d.a >= '1' RETURN d";

    irs::Or expected;
    expected.add<irs::by_range>()
            .field("a")
            .include<irs::Bound::MIN>(true).term<irs::Bound::MIN>("1");

    assertFilterSuccess(queryString, expected);
  }

  // complex attribute name, string
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c >= '1' RETURN d";

    irs::Or expected;
    expected.add<irs::by_range>()
            .field("a.b.c")
            .include<irs::Bound::MIN>(true).term<irs::Bound::MIN>("1");

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, true
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c >= true RETURN d";

    irs::Or expected;
    expected.add<irs::by_range>()
            .field(mangleBool("a.b.c"))
            .include<irs::Bound::MIN>(true).term<irs::Bound::MIN>(irs::boolean_token_stream::value_true());

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, false
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.bool >= false RETURN d";

    irs::Or expected;
    expected.add<irs::by_range>()
            .field(mangleBool("a.b.c.bool"))
            .include<irs::Bound::MIN>(true).term<irs::Bound::MIN>(irs::boolean_token_stream::value_false());

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, null
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.nil >= null RETURN d";

    irs::Or expected;
    expected.add<irs::by_range>()
            .field(mangleNull("a.b.c.nil"))
            .include<irs::Bound::MIN>(true).term<irs::Bound::MIN>(irs::null_token_stream::value_null());

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, numeric
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.numeric >= 13 RETURN d";

    irs::numeric_token_stream stream;
    stream.reset(13.);

    irs::Or expected;
    expected.add<irs::by_granular_range>()
            .field(mangleNumeric("a.b.c.numeric"))
            .include<irs::Bound::MIN>(true).insert<irs::Bound::MIN>(stream);

    assertFilterSuccess(queryString, expected);
  }

  // invalid attribute access
  assertFilterFail("FOR d IN collection FILTER d >= '1' RETURN d");
}

SECTION("BinaryGT") {
  // simple string attribute
  {
    std::string const queryString = "FOR d IN collection FILTER d.a > '1' RETURN d";

    irs::Or expected;
    expected.add<irs::by_range>()
            .field("a")
            .include<irs::Bound::MIN>(false).term<irs::Bound::MIN>("1");

    assertFilterSuccess(queryString, expected);
  }

  // complex attribute name, string
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c > '1' RETURN d";

    irs::Or expected;
    expected.add<irs::by_range>()
            .field("a.b.c")
            .include<irs::Bound::MIN>(false).term<irs::Bound::MIN>("1");

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, true
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c > true RETURN d";

    irs::Or expected;
    expected.add<irs::by_range>()
            .field(mangleBool("a.b.c"))
            .include<irs::Bound::MIN>(false).term<irs::Bound::MIN>(irs::boolean_token_stream::value_true());

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, false
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.bool > false RETURN d";

    irs::Or expected;
    expected.add<irs::by_range>()
            .field(mangleBool("a.b.c.bool"))
            .include<irs::Bound::MIN>(false).term<irs::Bound::MIN>(irs::boolean_token_stream::value_false());

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, null
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.nil > null RETURN d";

    irs::Or expected;
    expected.add<irs::by_range>()
            .field(mangleNull("a.b.c.nil"))
            .include<irs::Bound::MIN>(false).term<irs::Bound::MIN>(irs::null_token_stream::value_null());

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, numeric
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.numeric > 13 RETURN d";

    irs::numeric_token_stream stream;
    stream.reset(13.);

    irs::Or expected;
    expected.add<irs::by_granular_range>()
            .field(mangleNumeric("a.b.c.numeric"))
            .include<irs::Bound::MIN>(false).insert<irs::Bound::MIN>(stream);

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, floating
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.numeric > 13.5 RETURN d";

    irs::numeric_token_stream stream;
    stream.reset(13.5);

    irs::Or expected;
    expected.add<irs::by_granular_range>()
            .field(mangleNumeric("a.b.c.numeric"))
            .include<irs::Bound::MIN>(false).insert<irs::Bound::MIN>(stream);

    assertFilterSuccess(queryString, expected);
  }

  // invalid attribute access
  assertFilterFail("FOR d IN collection FILTER d > '1' RETURN d");
}

SECTION("BinaryLE") {
  // simple string attribute
  {
    std::string const queryString = "FOR d IN collection FILTER d.a <= '1' RETURN d";

    irs::Or expected;
    expected.add<irs::by_range>()
            .field("a")
            .include<irs::Bound::MAX>(true).term<irs::Bound::MAX>("1");

    assertFilterSuccess(queryString, expected);
  }

  // complex attribute name, string
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c <= '1' RETURN d";

    irs::Or expected;
    expected.add<irs::by_range>()
            .field("a.b.c")
            .include<irs::Bound::MAX>(true).term<irs::Bound::MAX>("1");

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, true
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c <= true RETURN d";

    irs::Or expected;
    expected.add<irs::by_range>()
            .field(mangleBool("a.b.c"))
            .include<irs::Bound::MAX>(true).term<irs::Bound::MAX>(irs::boolean_token_stream::value_true());

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, false
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.bool <= false RETURN d";

    irs::Or expected;
    expected.add<irs::by_range>()
            .field(mangleBool("a.b.c.bool"))
            .include<irs::Bound::MAX>(true).term<irs::Bound::MAX>(irs::boolean_token_stream::value_false());

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, null
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.nil <= null RETURN d";

    irs::Or expected;
    expected.add<irs::by_range>()
            .field(mangleNull("a.b.c.nil"))
            .include<irs::Bound::MAX>(true).term<irs::Bound::MAX>(irs::null_token_stream::value_null());

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, numeric
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.numeric <= 13 RETURN d";

    irs::numeric_token_stream stream;
    stream.reset(13.);

    irs::Or expected;
    expected.add<irs::by_granular_range>()
            .field(mangleNumeric("a.b.c.numeric"))
            .include<irs::Bound::MAX>(true).insert<irs::Bound::MAX>(stream);

    assertFilterSuccess(queryString, expected);
  }

  // invalid attribute access
  assertFilterFail("FOR d IN collection FILTER d <= '1' RETURN d");
}

SECTION("BinaryLT") {
  // simple string attribute
  {
    std::string const queryString = "FOR d IN collection FILTER d.a < '1' RETURN d";

    irs::Or expected;
    expected.add<irs::by_range>()
            .field("a")
            .include<irs::Bound::MAX>(false).term<irs::Bound::MAX>("1");

    assertFilterSuccess(queryString, expected);
  }

  // complex attribute name, string
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c < '1' RETURN d";

    irs::Or expected;
    expected.add<irs::by_range>()
            .field("a.b.c")
            .include<irs::Bound::MAX>(false).term<irs::Bound::MAX>("1");

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, true
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c < true RETURN d";

    irs::Or expected;
    expected.add<irs::by_range>()
            .field(mangleBool("a.b.c"))
            .include<irs::Bound::MAX>(false).term<irs::Bound::MAX>(irs::boolean_token_stream::value_true());

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, false
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.bool < false RETURN d";

    irs::Or expected;
    expected.add<irs::by_range>()
            .field(mangleBool("a.b.c.bool"))
            .include<irs::Bound::MAX>(false).term<irs::Bound::MAX>(irs::boolean_token_stream::value_false());

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, null
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.nil < null RETURN d";

    irs::Or expected;
    expected.add<irs::by_range>()
            .field(mangleNull("a.b.c.nil"))
            .include<irs::Bound::MAX>(false).term<irs::Bound::MAX>(irs::null_token_stream::value_null());

    assertFilterSuccess(queryString, expected);
  }

  // complex boolean attribute, numeric
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c.numeric < 13 RETURN d";

    irs::numeric_token_stream stream;
    stream.reset(13.);

    irs::Or expected;
    expected.add<irs::by_granular_range>()
            .field(mangleNumeric("a.b.c.numeric"))
            .include<irs::Bound::MAX>(false).insert<irs::Bound::MAX>(stream);

    assertFilterSuccess(queryString, expected);
  }

  // invalid attribute access
  assertFilterFail("FOR d IN collection FILTER d < '1' RETURN d");
}

SECTION("BinaryOr") {
  // string and string
  {
    std::string const queryString = "FOR d IN collection FILTER d.a == '1' or d.b == '2' RETURN d";

    irs::Or expected;
    auto& root = expected.add<irs::Or>();
    root.add<irs::by_term>().field("a").term("1");
    root.add<irs::by_term>().field("b").term("2");

    assertFilterSuccess(queryString, expected);
  }

  // string or string
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c < '1' or d.c.b.a == '2' RETURN d";

    irs::Or expected;
    auto& root = expected.add<irs::Or>();
    root.add<irs::by_range>()
        .field("a.b.c")
        .include<irs::Bound::MAX>(false).term<irs::Bound::MAX>("1");
    root.add<irs::by_term>().field("c.b.a").term("2");

    assertFilterSuccess(queryString, expected);
  }

  // bool and null
  {
    std::string const queryString = "FOR d IN collection FILTER k.b.c > false or d.a.b.c == null RETURN d";

    irs::Or expected;
    auto& root = expected.add<irs::Or>();
    root.add<irs::by_range>()
        .field(mangleBool("b.c"))
        .include<irs::Bound::MIN>(false).term<irs::Bound::MIN>(irs::boolean_token_stream::value_false());
    root.add<irs::by_term>().field(mangleNull("a.b.c")).term(irs::null_token_stream::value_null());

    assertFilterSuccess(queryString, expected);
  }

  // numeric range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c > 15 or d.a.b.c < 40 RETURN d";

    irs::numeric_token_stream minTerm; minTerm.reset(15.);
    irs::numeric_token_stream maxTerm; maxTerm.reset(40.);

    irs::Or expected;
    auto& root = expected.add<irs::Or>();
    root.add<irs::by_granular_range>()
        .field(mangleNumeric("a.b.c"))
        .include<irs::Bound::MIN>(false).insert<irs::Bound::MIN>(minTerm);
    root.add<irs::by_granular_range>()
        .field(mangleNumeric("a.b.c"))
        .include<irs::Bound::MAX>(false).insert<irs::Bound::MAX>(maxTerm);

    assertFilterSuccess(queryString, expected);
  }

  // numeric range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c >= 15 or d.a.b.c < 40 RETURN d";

    irs::numeric_token_stream minTerm; minTerm.reset(15.);
    irs::numeric_token_stream maxTerm; maxTerm.reset(40.);

    irs::Or expected;
    auto& root = expected.add<irs::Or>();
    root.add<irs::by_granular_range>()
        .field(mangleNumeric("a.b.c"))
        .include<irs::Bound::MIN>(true).insert<irs::Bound::MIN>(minTerm);
    root.add<irs::by_granular_range>()
        .field(mangleNumeric("a.b.c"))
        .include<irs::Bound::MAX>(false).insert<irs::Bound::MAX>(maxTerm);

    assertFilterSuccess(queryString, expected);
  }

  // numeric range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c >= 15 or d.a.b.c <= 40 RETURN d";

    irs::numeric_token_stream minTerm; minTerm.reset(15.);
    irs::numeric_token_stream maxTerm; maxTerm.reset(40.);

    irs::Or expected;
    auto& root = expected.add<irs::Or>();
    root.add<irs::by_granular_range>()
        .field(mangleNumeric("a.b.c"))
        .include<irs::Bound::MIN>(true).insert<irs::Bound::MIN>(minTerm);
    root.add<irs::by_granular_range>()
        .field(mangleNumeric("a.b.c"))
        .include<irs::Bound::MAX>(true).insert<irs::Bound::MAX>(maxTerm);

    assertFilterSuccess(queryString, expected);
  }

  // numeric range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c > 15 or d.a.b.c <= 40 RETURN d";

    irs::numeric_token_stream minTerm; minTerm.reset(15.);
    irs::numeric_token_stream maxTerm; maxTerm.reset(40.);

    irs::Or expected;
    auto& root = expected.add<irs::Or>();
    root.add<irs::by_granular_range>()
        .field(mangleNumeric("a.b.c"))
        .include<irs::Bound::MIN>(false).insert<irs::Bound::MIN>(minTerm);
    root.add<irs::by_granular_range>()
        .field(mangleNumeric("a.b.c"))
        .include<irs::Bound::MAX>(true).insert<irs::Bound::MAX>(maxTerm);

    assertFilterSuccess(queryString, expected);
  }
}

SECTION("BinaryAnd") {
  // string and string
  {
    std::string const queryString = "FOR d IN collection FILTER d.a == '1' and d.b == '2' RETURN d";

    irs::Or expected;
    auto& root = expected.add<irs::And>();
    root.add<irs::by_term>().field("a").term("1");
    root.add<irs::by_term>().field("b").term("2");

    assertFilterSuccess(queryString, expected);
  }

  // string and string
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c < '1' and d.c.b.a == '2' RETURN d";

    irs::Or expected;
    auto& root = expected.add<irs::And>();
    root.add<irs::by_range>()
        .field("a.b.c")
        .include<irs::Bound::MAX>(false).term<irs::Bound::MAX>("1");
    root.add<irs::by_term>().field("c.b.a").term("2");

    assertFilterSuccess(queryString, expected);
  }

  // bool and null
  {
    std::string const queryString = "FOR d IN collection FILTER k.b.c > false and d.a.b.c == null RETURN d";

    irs::Or expected;
    auto& root = expected.add<irs::And>();
    root.add<irs::by_range>()
        .field(mangleBool("b.c"))
        .include<irs::Bound::MIN>(false).term<irs::Bound::MIN>(irs::boolean_token_stream::value_false());
    root.add<irs::by_term>().field(mangleNull("a.b.c")).term(irs::null_token_stream::value_null());

    assertFilterSuccess(queryString, expected);
  }

  // numeric range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c > 15 and d.a.b.c < 40 RETURN d";

    irs::numeric_token_stream minTerm; minTerm.reset(15.);
    irs::numeric_token_stream maxTerm; maxTerm.reset(40.);

    irs::Or expected;
    auto& range = expected.add<irs::by_granular_range>();
    range.field(mangleNumeric("a.b.c"))
        .include<irs::Bound::MIN>(false).insert<irs::Bound::MIN>(minTerm)
        .include<irs::Bound::MAX>(false).insert<irs::Bound::MAX>(maxTerm);

    assertFilterSuccess(queryString, expected);
  }

  // numeric range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c >= 15 and d.a.b.c < 40 RETURN d";

    irs::numeric_token_stream minTerm; minTerm.reset(15.);
    irs::numeric_token_stream maxTerm; maxTerm.reset(40.);

    irs::Or expected;
    auto& range = expected.add<irs::by_granular_range>();
    range.field(mangleNumeric("a.b.c"))
        .include<irs::Bound::MIN>(true).insert<irs::Bound::MIN>(minTerm)
        .include<irs::Bound::MAX>(false).insert<irs::Bound::MAX>(maxTerm);

    assertFilterSuccess(queryString, expected);
  }

  // numeric range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c >= 15 and d.a.b.c <= 40 RETURN d";

    irs::numeric_token_stream minTerm; minTerm.reset(15.);
    irs::numeric_token_stream maxTerm; maxTerm.reset(40.);

    irs::Or expected;
    auto& range = expected.add<irs::by_granular_range>();
    range.field(mangleNumeric("a.b.c"))
        .include<irs::Bound::MIN>(true).insert<irs::Bound::MIN>(minTerm)
        .include<irs::Bound::MAX>(true).insert<irs::Bound::MAX>(maxTerm);

    assertFilterSuccess(queryString, expected);
  }

  // numeric range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c > 15 and d.a.b.c <= 40 RETURN d";

    irs::numeric_token_stream minTerm; minTerm.reset(15.);
    irs::numeric_token_stream maxTerm; maxTerm.reset(40.);

    irs::Or expected;
    auto& range = expected.add<irs::by_granular_range>();
    range.field(mangleNumeric("a.b.c"))
        .include<irs::Bound::MIN>(false).insert<irs::Bound::MIN>(minTerm)
        .include<irs::Bound::MAX>(true).insert<irs::Bound::MAX>(maxTerm);

    assertFilterSuccess(queryString, expected);
  }

  // string range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c > '15' and d.a.b.c < '40' RETURN d";

    irs::Or expected;
    auto& range = expected.add<irs::by_range>();
    range.field("a.b.c")
        .include<irs::Bound::MIN>(false).term<irs::Bound::MIN>("15")
        .include<irs::Bound::MAX>(false).term<irs::Bound::MAX>("40");

    assertFilterSuccess(queryString, expected);
  }

  // string range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c >= '15' and d.a.b.c < '40' RETURN d";

    irs::Or expected;
    auto& range = expected.add<irs::by_range>();
    range.field("a.b.c")
        .include<irs::Bound::MIN>(true).term<irs::Bound::MIN>("15")
        .include<irs::Bound::MAX>(false).term<irs::Bound::MAX>("40");

    assertFilterSuccess(queryString, expected);
  }

  // string range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c >= '15' and d.a.b.c <= '40' RETURN d";

    irs::Or expected;
    auto& range = expected.add<irs::by_range>();
    range.field("a.b.c")
        .include<irs::Bound::MIN>(true).term<irs::Bound::MIN>("15")
        .include<irs::Bound::MAX>(true).term<irs::Bound::MAX>("40");

    assertFilterSuccess(queryString, expected);
  }

  // string range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c > '15' and d.a.b.c <= '40' RETURN d";

    irs::Or expected;
    auto& range = expected.add<irs::by_range>();
    range.field("a.b.c")
        .include<irs::Bound::MIN>(false).term<irs::Bound::MIN>("15")
        .include<irs::Bound::MAX>(true).term<irs::Bound::MAX>("40");

    assertFilterSuccess(queryString, expected);
  }

  // heterogeneous range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c >= '15' and d.a.b.c < 40 RETURN d";

    irs::numeric_token_stream maxTerm; maxTerm.reset(40.);

    irs::Or expected;
    auto& root = expected.add<irs::And>();
    root.add<irs::by_range>()
        .field("a.b.c")
        .include<irs::Bound::MIN>(true).term<irs::Bound::MIN>("15");
    root.add<irs::by_granular_range>()
        .field(mangleNumeric("a.b.c"))
        .include<irs::Bound::MAX>(false).insert<irs::Bound::MAX>(maxTerm);

    assertFilterSuccess(queryString, expected);
  }

  // heterogeneous range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c > 15 and d.a.b.c <= '40' RETURN d";

    irs::numeric_token_stream minTerm; minTerm.reset(15.);
    irs::numeric_token_stream maxTerm; maxTerm.reset(40.);

    irs::Or expected;
    auto& root = expected.add<irs::And>();
    root.add<irs::by_granular_range>()
        .field(mangleNumeric("a.b.c"))
        .include<irs::Bound::MIN>(false).insert<irs::Bound::MIN>(minTerm);
    root.add<irs::by_range>()
        .field("a.b.c")
        .include<irs::Bound::MAX>(true).term<irs::Bound::MAX>("40");

    assertFilterSuccess(queryString, expected);
  }

  // heterogeneous range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c >= false and d.a.b.c <= 40 RETURN d";

    irs::numeric_token_stream maxTerm; maxTerm.reset(40.);

    irs::Or expected;
    auto& root = expected.add<irs::And>();
    root.add<irs::by_range>()
        .field(mangleBool("a.b.c"))
        .include<irs::Bound::MIN>(true).term<irs::Bound::MIN>(irs::boolean_token_stream::value_false());
    root.add<irs::by_granular_range>()
        .field(mangleNumeric("a.b.c"))
        .include<irs::Bound::MAX>(true).insert<irs::Bound::MAX>(maxTerm);

    assertFilterSuccess(queryString, expected);
  }

  // heterogeneous range
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c > null and d.a.b.c <= 40.5 RETURN d";

    irs::numeric_token_stream maxTerm; maxTerm.reset(40.5);

    irs::Or expected;
    auto& root = expected.add<irs::And>();
    root.add<irs::by_range>()
        .field(mangleNull("a.b.c"))
        .include<irs::Bound::MIN>(false).term<irs::Bound::MIN>(irs::null_token_stream::value_null());
    root.add<irs::by_granular_range>()
        .field(mangleNumeric("a.b.c"))
        .include<irs::Bound::MAX>(true).insert<irs::Bound::MAX>(maxTerm);

    assertFilterSuccess(queryString, expected);
  }

  // range with different references
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c >= '15' and k.a.b.c < 40 RETURN d";

    irs::numeric_token_stream maxTerm; maxTerm.reset(40.);

    irs::Or expected;
    auto& root = expected.add<irs::And>();
    root.add<irs::by_range>()
        .field("a.b.c")
        .include<irs::Bound::MIN>(true).term<irs::Bound::MIN>("15");
    root.add<irs::by_granular_range>()
        .field(mangleNumeric("a.b.c"))
        .include<irs::Bound::MAX>(false).insert<irs::Bound::MAX>(maxTerm);

    assertFilterSuccess(queryString, expected);
  }

  // range with different references
  {
    std::string const queryString = "FOR d IN collection FILTER k.a.b.c > 15 and d.a.b.c <= '40' RETURN d";

    irs::numeric_token_stream minTerm; minTerm.reset(15.);
    irs::numeric_token_stream maxTerm; maxTerm.reset(40.);

    irs::Or expected;
    auto& root = expected.add<irs::And>();
    root.add<irs::by_granular_range>()
        .field(mangleNumeric("a.b.c"))
        .include<irs::Bound::MIN>(false).insert<irs::Bound::MIN>(minTerm);
    root.add<irs::by_range>()
        .field("a.b.c")
        .include<irs::Bound::MAX>(true).term<irs::Bound::MAX>("40");

    assertFilterSuccess(queryString, expected);
  }

  // range with different references
  {
    std::string const queryString = "FOR d IN collection FILTER d.a.b.c >= false and k.a.b.c <= 40 RETURN d";

    irs::numeric_token_stream maxTerm; maxTerm.reset(40.);

    irs::Or expected;
    auto& root = expected.add<irs::And>();
    root.add<irs::by_range>()
        .field(mangleBool("a.b.c"))
        .include<irs::Bound::MIN>(true).term<irs::Bound::MIN>(irs::boolean_token_stream::value_false());
    root.add<irs::by_granular_range>()
        .field(mangleNumeric("a.b.c"))
        .include<irs::Bound::MAX>(true).insert<irs::Bound::MAX>(maxTerm);

    assertFilterSuccess(queryString, expected);
  }

  // range with different references
  {
    std::string const queryString = "FOR d IN collection FILTER k.a.b.c > null and d.a.b.c <= 40.5 RETURN d";

    irs::numeric_token_stream maxTerm; maxTerm.reset(40.5);

    irs::Or expected;
    auto& root = expected.add<irs::And>();
    root.add<irs::by_range>()
        .field(mangleNull("a.b.c"))
        .include<irs::Bound::MIN>(false).term<irs::Bound::MIN>(irs::null_token_stream::value_null());
    root.add<irs::by_granular_range>()
        .field(mangleNumeric("a.b.c"))
        .include<irs::Bound::MAX>(true).insert<irs::Bound::MAX>(maxTerm);

    assertFilterSuccess(queryString, expected);
  }
}

SECTION("Value") {
  // string value == true
  {
    std::string const queryString = "FOR d IN collection FILTER '1' RETURN d";

    irs::Or expected;
    expected.add<irs::all>();

    assertFilterSuccess(queryString, expected);
  }

  // true value
  {
    std::string const queryString = "FOR d IN collection FILTER true RETURN d";

    irs::Or expected;
    expected.add<irs::all>();

    assertFilterSuccess(queryString, expected);
  }

  // string empty value == false
  {
    std::string const queryString = "FOR d IN collection FILTER '' RETURN d";

    irs::Or expected;
    expected.add<irs::empty>();

    assertFilterSuccess(queryString, expected);
  }

  // false
  {
    std::string const queryString = "FOR d IN collection FILTER false RETURN d";

    irs::Or expected;
    expected.add<irs::empty>();

    assertFilterSuccess(queryString, expected);
  }

  // null == value
  {
    std::string const queryString = "FOR d IN collection FILTER null RETURN d";

    irs::Or expected;
    expected.add<irs::empty>();

    assertFilterSuccess(queryString, expected);
  }

  // non zero numeric value
  {
    std::string const queryString = "FOR d IN collection FILTER 1 RETURN d";

    irs::Or expected;
    expected.add<irs::all>();

    assertFilterSuccess(queryString, expected);
  }

  // zero numeric value
  {
    std::string const queryString = "FOR d IN collection FILTER 0 RETURN d";

    irs::Or expected;
    expected.add<irs::empty>();

    assertFilterSuccess(queryString, expected);
  }

  // zero floating value
  {
    std::string const queryString = "FOR d IN collection FILTER 0.0 RETURN d";

    irs::Or expected;
    expected.add<irs::empty>();

    assertFilterSuccess(queryString, expected);
  }

  // non zero floating value
  {
    std::string const queryString = "FOR d IN collection FILTER 0.1 RETURN d";

    irs::Or expected;
    expected.add<irs::all>();

    assertFilterSuccess(queryString, expected);
  }

  // Array == true
  {
    std::string const queryString = "FOR d IN collection FILTER [] RETURN d";

    irs::Or expected;
    expected.add<irs::all>();

    assertFilterSuccess(queryString, expected);
  }

  // Range == true
  {
    std::string const queryString = "FOR d IN collection FILTER 1..2 RETURN d";

    irs::Or expected;
    expected.add<irs::all>();

    assertFilterSuccess(queryString, expected);
  }

  // Object == true
  {
    std::string const queryString = "FOR d IN collection FILTER {} RETURN d";

    irs::Or expected;
    expected.add<irs::all>();

    assertFilterSuccess(queryString, expected);
  }

  // reference
  assertFilterFail("FOR d IN collection FILTER d RETURN d");
}

SECTION("Phrase") {
  // without offset
  // quick
  {
    std::string const queryString = "FOR d IN VIEW myView FILTER ir::phrase(d.name, 'quick') RETURN d";

    irs::Or expected;
    auto& phrase = expected.add<irs::by_phrase>();
    phrase.field("name").push_back("quick");

    assertFilterSuccess(queryString, expected);

    // invalid attribute access
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(d, 'quick') RETURN d");
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase('d.name', 'quick') RETURN d");
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(123, 'quick') RETURN d");
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(123.5, 'quick') RETURN d");
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(null, 'quick') RETURN d");
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(true, 'quick') RETURN d");
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(false, 'quick') RETURN d");
  }

  // with offset
  // quick brown
  {
    std::string const queryString = "FOR d IN VIEW myView FILTER ir::phrase(d.name, 'quick', 0, 'brown') RETURN d";

    irs::Or expected;
    auto& phrase = expected.add<irs::by_phrase>();
    phrase.field("name").push_back("quick").push_back("brown");

    assertFilterSuccess(queryString, expected);

    // wrong offset argument
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(d.name, 'quick', '0', 'brown') RETURN d");
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(d.name, 'quick', null, 'brown') RETURN d");
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(d.name, 'quick', true, 'brown') RETURN d");
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(d.name, 'quick', false, 'brown') RETURN d");
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(d.name, 'quick', d.name, 'brown') RETURN d");
  }

  // with offset, complex name
  // quick <...> <...> <...> <...> <...> brown
  {
    std::string const queryString = "FOR d IN VIEW myView FILTER ir::phrase(d.obj.name, 'quick', 5, 'brown') RETURN d";

    irs::Or expected;
    auto& phrase = expected.add<irs::by_phrase>();
    phrase.field("obj.name").push_back("quick").push_back("brown", 5);

    assertFilterSuccess(queryString, expected);
  }

  // with floating offset, complex name
  // quick <...> <...> <...> <...> <...> brown
  {
    std::string const queryString = "FOR d IN VIEW myView FILTER ir::phrase(d.obj.name, 'quick', 5.5, 'brown') RETURN d";

    irs::Or expected;
    auto& phrase = expected.add<irs::by_phrase>();
    phrase.field("obj.name").push_back("quick").push_back("brown", 5);

    assertFilterSuccess(queryString, expected);
  }

  // multiple offsets, complex name
  // quick <...> <...> <...> brown <...> <...> fox jumps
  {
    std::string const queryString = "FOR d IN VIEW myView FILTER ir::phrase(d.obj.properties.id.name, 'quick', 3, 'brown', 2, 'fox', 0, 'jumps') RETURN d";

    irs::Or expected;
    auto& phrase = expected.add<irs::by_phrase>();
    phrase.field("obj.properties.id.name")
          .push_back("quick")
          .push_back("brown", 3)
          .push_back("fox", 2)
          .push_back("jumps");

    assertFilterSuccess(queryString, expected);

    // wrong value
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(d.obj.properties.id.name, 'quick', 3, d.brown, 2, 'fox', 0, 'jumps') RETURN d");
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(d.obj.properties.id.name, 'quick', 3, 2, 2, 'fox', 0, 'jumps') RETURN d");
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(d.obj.properties.id.name, 'quick', 3, 2.5, 2, 'fox', 0, 'jumps') RETURN d");
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(d.obj.properties.id.name, 'quick', 3, null, 2, 'fox', 0, 'jumps') RETURN d");
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(d.obj.properties.id.name, 'quick', 3, true, 2, 'fox', 0, 'jumps') RETURN d");
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(d.obj.properties.id.name, 'quick', 3, false, 2, 'fox', 0, 'jumps') RETURN d");
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(d.obj.properties.id.name, 'quick', 3, 'brown', 2, 'fox', 0, d) RETURN d");

    // wrong offset argument
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(d.obj.properties.id.name, 'quick', 3, 'brown', '2', 'fox', 0, 'jumps') RETURN d");
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(d.obj.properties.id.name, 'quick', 3, 'brown', null, 'fox', 0, 'jumps') RETURN d");
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(d.obj.properties.id.name, 'quick', 3, 'brown', true, 'fox', 0, 'jumps') RETURN d");
    assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(d.obj.properties.id.name, 'quick', 3, 'brown', false, 'fox', 0, 'jumps') RETURN d");
  }

  // wrong number of arguments
  assertFilterFail("FOR d IN VIEW myView FILTER ir::phrase(d.name, 'quick', 3) RETURN d");
}

SECTION("StartsWith") {
  // without scoring limit
  {
    std::string const queryString = "FOR d IN VIEW myView FILTER ir::starts_with(d.name, 'abc') RETURN d";

    irs::Or expected;
    auto& prefix = expected.add<irs::by_prefix>();
    prefix.field("name").term("abc");
    prefix.scored_terms_limit(128);

    assertFilterSuccess(queryString, expected);
  }

  // without scoring limit, complex name
  {
    std::string const queryString = "FOR d IN VIEW myView FILTER ir::starts_with(d.obj.properties.name, 'abc') RETURN d";

    irs::Or expected;
    auto& prefix = expected.add<irs::by_prefix>();
    prefix.field("obj.properties.name").term("abc");
    prefix.scored_terms_limit(128);

    assertFilterSuccess(queryString, expected);
  }

  // with scoring limit (int)
  {
    std::string const queryString = "FOR d IN VIEW myView FILTER ir::starts_with(d.name, 'abc', 1024) RETURN d";

    irs::Or expected;
    auto& prefix = expected.add<irs::by_prefix>();
    prefix.field("name").term("abc");
    prefix.scored_terms_limit(1024);

    assertFilterSuccess(queryString, expected);
  }

  // with scoring limit (double)
  {
    std::string const queryString = "FOR d IN VIEW myView FILTER ir::starts_with(d.name, 'abc', 100.5) RETURN d";

    irs::Or expected;
    auto& prefix = expected.add<irs::by_prefix>();
    prefix.field("name").term("abc");
    prefix.scored_terms_limit(100);

    assertFilterSuccess(queryString, expected);
  }

  // invalid attribute access
  assertFilterFail("FOR d IN VIEW myView FILTER ir::starts_with(d, 'abc') RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER ir::starts_with('d.name', 'abc') RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER ir::starts_with(123, 'abc') RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER ir::starts_with(123.5, 'abc') RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER ir::starts_with(null, 'abc') RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER ir::starts_with(true, 'abc') RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER ir::starts_with(false, 'abc') RETURN d");

  // invalid value
  assertFilterFail("FOR d IN VIEW myView FILTER ir::starts_with(d.name, 1) RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER ir::starts_with(d.name, 1.5) RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER ir::starts_with(d.name, true) RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER ir::starts_with(d.name, false) RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER ir::starts_with(d.name, null) RETURN d");

  // invalid scoring limit
  assertFilterFail("FOR d IN VIEW myView FILTER ir::starts_with(d.name, 'abc', '1024') RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER ir::starts_with(d.name, 'abc', true) RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER ir::starts_with(d.name, 'abc', false) RETURN d");
  assertFilterFail("FOR d IN VIEW myView FILTER ir::starts_with(d.name, 'abc', null) RETURN d");
}

}

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------
