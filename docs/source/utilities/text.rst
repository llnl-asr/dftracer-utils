Text
==============

Text processing utilities for splitting and filtering.

.. code-block:: cpp

   #include <dftracer/utils/utilities/text/line_splitter.h>
   #include <dftracer/utils/utilities/text/line_filter.h>

LineSplitterUtility
-------------------

Splits text into lines with line numbers.

.. code-block:: cpp

   LineSplitterUtility splitter;

   Text content{"line1\nline2\nline3"};
   Lines lines = splitter.process(content);

   for (const auto& line : lines) {
       std::cout << line.line_number << ": " << line.content << "\n";
   }
   // Output:
   // 1: line1
   // 2: line2
   // 3: line3

LineFilterUtility
-----------------

Filters a single line based on predicate.

.. code-block:: cpp

   struct FilterableLine {
       Line line;
       std::function<bool(const Line&)> predicate;
   };

   LineFilterUtility filter;

   FilterableLine input{
       Line{"error: something failed", 42},
       [](const Line& l) { return l.content.find("error") != std::string_view::npos; }
   };

   std::optional<Line> result = filter.process(input);
   if (result) {
       std::cout << "Matched: " << result->content << "\n";
   }

MultiLinesFilterUtility
-----------------------

Batch filtering of multiple lines.

.. code-block:: cpp

   MultiLinesFilterUtility filter;

   // Set predicate
   filter.set_predicate([](const Line& l) {
       return l.content.find("TRACE") != std::string_view::npos;
   });

   Lines input = /* ... */;
   Lines filtered = filter.process(input);

   std::cout << "Filtered " << filtered.size() << " lines\n";
