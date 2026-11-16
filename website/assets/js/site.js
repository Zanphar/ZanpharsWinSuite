// Zanphar's WinSuite - site behaviour. No dependencies, no network calls.
(function () {
  "use strict";

  // ---------------------------------------------------------------- nav menu
  var toggle = document.querySelector(".nav-toggle");
  var nav = document.querySelector(".nav");
  if (toggle && nav) {
    toggle.addEventListener("click", function () {
      var open = nav.classList.toggle("open");
      toggle.setAttribute("aria-expanded", open ? "true" : "false");
    });
    nav.addEventListener("click", function (event) {
      if (event.target.closest("a")) {
        nav.classList.remove("open");
        toggle.setAttribute("aria-expanded", "false");
      }
    });
  }

  // --------------------------------------------------------- current page
  var page = document.body.getAttribute("data-page");
  if (page) {
    var current = document.querySelector('.nav a[data-nav="' + page + '"]');
    if (current) current.setAttribute("aria-current", "page");
  }

  // ------------------------------------------------------------- reveal in
  var revealables = document.querySelectorAll(".reveal");
  if (!("IntersectionObserver" in window)) {
    revealables.forEach(function (el) { el.classList.add("in"); });
  } else {
    var observer = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        if (entry.isIntersecting) {
          entry.target.classList.add("in");
          observer.unobserve(entry.target);
        }
      });
    }, { rootMargin: "0px 0px -8% 0px", threshold: 0.05 });
    revealables.forEach(function (el) { observer.observe(el); });
  }

  // -------------------------------------------------------------- lightbox
  var lightbox = document.querySelector("dialog.lightbox");
  if (lightbox) {
    var image = lightbox.querySelector("img");
    var caption = lightbox.querySelector("[data-caption]");
    document.querySelectorAll("[data-zoom]").forEach(function (button) {
      button.addEventListener("click", function () {
        var source = button.querySelector("img");
        if (!source) return;
        image.src = source.src;
        image.alt = source.alt;
        if (caption) caption.textContent = button.getAttribute("data-caption") || source.alt;
        if (typeof lightbox.showModal === "function") lightbox.showModal();
        else lightbox.setAttribute("open", "");
      });
    });
    lightbox.addEventListener("click", function (event) {
      if (event.target === lightbox || event.target.closest("[data-close]")) lightbox.close();
    });
    lightbox.addEventListener("close", function () {
      image.removeAttribute("src");
    });
  }

  // ------------------------------------------------------------ copy hash
  document.querySelectorAll("[data-copy]").forEach(function (button) {
    button.addEventListener("click", function () {
      var text = button.getAttribute("data-copy");
      var label = button.querySelector("span") || button;
      var original = label.textContent;
      function done(ok) {
        if (ok) {
          label.textContent = "Copied";
        } else {
          // No clipboard permission: select the value so Ctrl+C is one key away.
          label.textContent = "Select and copy";
          var target = button.parentElement && button.parentElement.querySelector("code");
          if (target && window.getSelection) {
            var range = document.createRange();
            range.selectNodeContents(target);
            var selection = window.getSelection();
            selection.removeAllRanges();
            selection.addRange(range);
          }
        }
        setTimeout(function () { label.textContent = original; }, 1800);
      }
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).then(function () { done(true); }, function () { done(false); });
      } else {
        var area = document.createElement("textarea");
        area.value = text;
        area.setAttribute("readonly", "");
        area.style.position = "fixed";
        area.style.opacity = "0";
        document.body.appendChild(area);
        area.select();
        var ok = false;
        try { ok = document.execCommand("copy"); } catch (e) { ok = false; }
        document.body.removeChild(area);
        done(ok);
      }
    });
  });

  // ------------------------------------------------------- footer year
  var year = document.querySelector("[data-year]");
  if (year) year.textContent = String(new Date().getFullYear());
})();
