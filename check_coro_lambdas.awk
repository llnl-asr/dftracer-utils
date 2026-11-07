function is_id(c) { return c != "" && index("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", c) > 0 }

function check_capture(k,    parts, m, j, item) {
  m = split(cap[k], parts, ",")
  for (j = 1; j <= m; j++) {
    item = parts[j]
    gsub(/[ \t]/, "", item)
    if (item == "" || item == "&" || item == "this" || item ~ /^&[A-Za-z_][A-Za-z_0-9]*$/) continue
    print FILENAME ":" capline[k] ": lambda with a by-value capture in a co_await"
    hits++
    return
  }
}

function feed(k, c) {
  if (st[k] == 1) {
    if (c == "]") { check_capture(k); st[k] = 2 } else cap[k] = cap[k] c
    return
  }
  if (st[k] == 2) {
    if (c == "{") { st[k] = 3; bd[k] = 1 }
    return
  }
  if (st[k] == 3) {
    if (c == "{") bd[k]++
    else if (c == "}" && --bd[k] == 0) st[k] = 0
    return
  }
  if (c == "[" && lambda_start) { st[k] = 1; cap[k] = ""; capline[k] = FNR; return }
  if (c == "(" || c == "[" || c == "{") d[k]++
  else if (c == ")" || c == "]" || c == "}") { if (--d[k] < 0) dead[k] = 1 }
  else if (c == ";" && d[k] == 0) dead[k] = 1
}

function feed_all(c,    k, live) {
  lambda_start = (prev == "(" || prev == "," || prev == "=" || prev == "{" || \
                  prev == "?" || prev == ":" || prev == "return" || prev == "co_await")
  live = 0
  for (k = 1; k <= nscan; k++) {
    if (dead[k]) continue
    feed(k, c)
    if (!dead[k]) live = 1
  }
  if (!live) nscan = 0
}

function word_end(w) {
  prev = w
  if (w == "co_await") {
    nscan++
    st[nscan] = 0; d[nscan] = 0; dead[nscan] = 0
  }
}

FNR == 1 { in_block = 0; nscan = 0; prev = "" }

{
  line = $0 " "
  n = length(line)
  word = ""
  i = 1
  while (i <= n) {
    c = substr(line, i, 1)
    two = substr(line, i, 2)
    if (in_block) {
      if (two == "*/") { in_block = 0; i += 2 } else i++
      continue
    }
    if (two == "/*") { in_block = 1; i += 2; continue }
    if (two == "//") break
    if (c == "\"" || c == "'") {
      if (word != "") { word_end(word); word = "" }
      q = c
      i++
      while (i <= n) {
        d2 = substr(line, i, 1)
        if (d2 == "\\") { i += 2; continue }
        if (d2 == q) break
        i++
      }
      i++
      prev = q
      continue
    }
    if (is_id(c)) { word = word c; feed_all(c); i++; continue }
    if (word != "") { word_end(word); word = "" }
    feed_all(c)
    if (c != " " && c != "\t") prev = c
    i++
  }
  if (word != "") word_end(word)
}

END { exit hits > 0 ? 1 : 0 }
