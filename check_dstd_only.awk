FNR == 1 { kept = 0 }
{
  line = $0
  if (line ~ /\/\/ keep std: /) { kept = (line ~ /^[ \t]*\/\//); next }
  if (kept) { kept = 0; next }
  sub(/\/\/.*$/, "", line)
  if (line ~ /(^|[^A-Za-z0-9_:])std::(string|string_view|vector|span|map|set|multimap|multiset|unordered_map|unordered_set|deque|list|queue|priority_queue|unique_ptr|make_unique|shared_ptr|make_shared|weak_ptr|optional|nullopt|variant|monostate|pair|make_pair|tuple|any|any_cast|function|thread|promise|future|shared_future|to_string|stoi|stol|stoll|stoul|stoull|stod|stof|stable_sort|ofstream|ifstream|ostringstream|stringstream|istringstream|to_chars|from_chars)([^A-Za-z0-9_]|$)/) {
    print FILENAME ":" FNR ": " $0
    hits++
  }
}
END { exit hits > 0 ? 1 : 0 }
