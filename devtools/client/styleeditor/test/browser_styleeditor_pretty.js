/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */
"use strict";

// Test that only minified sheets are automatically prettified,
// and that the pretty print button behaves as expected.

const TESTCASE_URI = TEST_BASE_HTTP + "minified.html";

const PRETTIFIED_CSS_TEXT = `
body {
  background:white;
}
div {
  font-size:4em;
  color:red
}
span {
  color:green;
  @media screen {
    background: blue;
    &>.myClass {
      padding: 1em
    }
  }
}
`.trimStart();

const INLINE_STYLESHEET_ORIGINAL_CSS_TEXT = `
body { background: red; }
div {
font-size: 5em;
color: red;
}`.trimStart();

const INLINE_STYLESHEET_PRETTIFIED_CSS_TEXT = `
body {
  background: red;
}
div {
  font-size: 5em;
  color: red;
}
`.trimStart();

add_task(async function () {
  // Use 2 spaces for indent
  await pushPref("devtools.editor.expandtab", true);
  await pushPref("devtools.editor.tabsize", 2);

  const { panel, ui } = await openStyleEditorForURL(TESTCASE_URI);
  is(ui.editors.length, 3, "Three sheets present.");

  info("Testing minified style sheet.");
  const minifiedEditor = await ui.editors[0].getSourceEditor();

  is(
    minifiedEditor.sourceEditor.getText(),
    PRETTIFIED_CSS_TEXT,
    "minified source has been prettified automatically"
  );

  const prettyPrintButton = panel.panelWindow.document.querySelector(
    ".style-editor-prettyPrintButton"
  );

  info("Attempt to prettify the stylesheet manually");
  let onEditorChange = minifiedEditor.sourceEditor.once("changes");
  EventUtils.synthesizeMouseAtCenter(prettyPrintButton, {}, panel.panelWindow);
  await onEditorChange;

  is(
    minifiedEditor.sourceEditor.getText(),
    PRETTIFIED_CSS_TEXT,
    "Re-prettifying the stylesheet did not change the text unnecessarily"
  );

  info("Selecting second, non-minified style sheet.");
  await ui.selectStyleSheet(ui.editors[1].styleSheet);

  const inlineEditor = ui.editors[1];
  is(
    inlineEditor.sourceEditor.getText(),
    INLINE_STYLESHEET_ORIGINAL_CSS_TEXT,
    "non-minified source has been left untouched"
  );
  ok(prettyPrintButton, "Pretty print button is displayed");
  ok(
    !prettyPrintButton.hasAttribute("disabled"),
    "Pretty print button is enabled"
  );
  is(
    prettyPrintButton.getAttribute("title"),
    "Pretty print style sheet",
    "Pretty print button has the expected title attribute"
  );

  onEditorChange = inlineEditor.sourceEditor.once("changes");
  EventUtils.synthesizeMouseAtCenter(prettyPrintButton, {}, panel.panelWindow);
  await onEditorChange;

  is(
    inlineEditor.sourceEditor.getText(),
    INLINE_STYLESHEET_PRETTIFIED_CSS_TEXT,
    "inline stylesheet was prettified as expected when clicking on pretty print button"
  );

  info("Attempt to prettify the inline stylesheet again");
  onEditorChange = inlineEditor.sourceEditor.once("changes");
  EventUtils.synthesizeMouseAtCenter(prettyPrintButton, {}, panel.panelWindow);
  await onEditorChange;

  is(
    inlineEditor.sourceEditor.getText(),
    INLINE_STYLESHEET_PRETTIFIED_CSS_TEXT,
    "Re-prettifying the inline stylesheet did not change the text unnecessarily"
  );

  info("Selecting original style sheet.");
  await ui.selectStyleSheet(ui.editors[2].styleSheet);
  ok(
    prettyPrintButton.hasAttribute("disabled"),
    "Pretty print button is disabled when selecting an original file"
  );
  await waitFor(
    () =>
      prettyPrintButton.getAttribute("title") ===
      "Can only pretty print CSS files"
  );
  ok(
    true,
    "Pretty print button has the expected title attribute when it's disabled"
  );
});

add_task(async function testSystemStylesheet() {
  const { ui, panel } = await openStyleEditorForURL("about:support");

  const formsEditor = ui.editors.find(
    editor => editor.friendlyName === "forms.css"
  );
  ok(!!formsEditor, "Found the editor for forms.css");

  info("Selecting system style sheet.");
  await ui.selectStyleSheet(formsEditor.styleSheet);

  ok(
    formsEditor.sourceEditor.config.readOnly,
    "The editor for system stylesheet is readonly"
  );

  const prettyPrintButton = panel.panelWindow.document.querySelector(
    ".style-editor-prettyPrintButton"
  );
  ok(prettyPrintButton, "Pretty print button is displayed");
  ok(
    prettyPrintButton.hasAttribute("disabled"),
    "Pretty print button is disabled"
  );
  await waitFor(
    () =>
      prettyPrintButton.getAttribute("title") ===
      "Can’t pretty print read-only style sheet."
  );

  ok(true, "Pretty print button has the expected title attribute");
});
