BEGIN { inblock = 0 }
FNR == 1 { inblock = 0 }
NR == FNR {
  n = split($0, f, "\t")
  if (f[1] == "T") typerow[f[2]] = f[3]
  else if (f[1] == "F") filetype[f[2], f[3]] = f[4]
  next
}
FILENAME != lastfile { lastfile = FILENAME; inblock = 0 }
function hit(key, text,    k) {
  if (key in typerow) { usedtype[key] = 1; return }
  k = FILENAME SUBSEP key
  if (k in filetype) { usedfile[k] = 1; return }
  print FILENAME ":" FNR ": " text "  (raw new of " key ": derive dstd::HeapObject or use dstd::make_unique)"
  hits++
}
function strip(s,    p1, p2, rest, q) {
  gsub(/"([^"\\]|\\.)*"/, "\"\"", s)
  gsub(/'([^'\\]|\\.)'/, "' '", s)
  if (inblock) {
    q = index(s, "*/")
    if (q == 0) return ""
    s = substr(s, q + 2)
    inblock = 0
  }
  for (;;) {
    p1 = index(s, "/*")
    p2 = index(s, "//")
    if (p2 && (!p1 || p2 < p1)) return substr(s, 1, p2 - 1)
    if (!p1) return s
    rest = substr(s, p1 + 2)
    q = index(rest, "*/")
    if (q) { s = substr(s, 1, p1 - 1) " " substr(rest, q + 2); continue }
    inblock = 1
    return substr(s, 1, p1 - 1)
  }
}
function type_key(tail,    depth, i, c, t) {
  depth = 0
  t = ""
  for (i = 1; i <= length(tail); i++) {
    c = substr(tail, i, 1)
    if (c == "<") depth++
    else if (c == ">") { if (depth == 0) break; depth-- }
    else if (depth == 0 && (c == "(" || c == "{" || c == "[" || c == ";" || c == "," || c == ")" || c == "=")) break
    t = t c
  }
  while (t ~ /<[^<>]*>/) sub(/<[^<>]*>/, "", t)
  gsub(/[ \t]+$/, "", t)
  sub(/^(const|struct|class)[ \t]+/, "", t)
  gsub(/.*::/, "", t)
  return t
}
{
  s = strip($0)
  if (s ~ /delete[ \t]*\[[ \t]*\]/ && s !~ /operator[ \t]+delete[ \t]*\[/) hit("delete[]", $0)
  off = 1
  while ((i = index(substr(s, off), "new")) > 0) {
    a = off + i - 1
    off = a + 3
    before = a > 1 ? substr(s, a - 1, 1) : " "
    after = substr(s, a + 3, 1)
    if (before ~ /[A-Za-z0-9_]/ || after ~ /[A-Za-z0-9_]/) continue
    if (substr(s, 1, a - 1) ~ /operator[ \t]*$/) continue
    tail = substr(s, a + 3)
    sub(/^[ \t]+/, "", tail)
    if (tail ~ /^\(/) {
      if (tail ~ /^\([ \t]*(std::)?nothrow[ \t]*\)/) sub(/^\([^)]*\)[ \t]*/, "", tail)
      else continue
    }
    if (tail == "") { if (substr(s, 1, a - 1) ~ /<$/) continue; hit("?", $0); continue }
    if (tail !~ /^[A-Za-z_:]/) continue
    hit(type_key(tail), $0)
  }
}
END {
  for (key in typerow)
    if (!(key in usedtype)) {
      print "stale allow row, no site makes " key " with new: " typerow[key]
      hits++
    }
  for (k in filetype)
    if (!(k in usedfile)) {
      split(k, p, SUBSEP)
      print "stale allow row, " p[1] " does not make " p[2] " with new: " filetype[k]
      hits++
    }
  exit hits > 0 ? 1 : 0
}
