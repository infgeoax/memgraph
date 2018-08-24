#pragma once

#include "database/graph_db.hpp"

namespace query {

class Interpreter;

/**
 * Read Evaluate Print Loop, for interacting with a database (the database in
 * the given database::GraphDb). Immediately starts the user-input loop and
 * interprets the entered queries.
 */
void Repl(database::GraphDb *, query::Interpreter *);

}  // namespace query
