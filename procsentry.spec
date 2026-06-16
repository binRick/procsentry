%global debug_package %{nil}

Name:           procsentry
Version:        0.1.0
Release:        1%{?dist}
Summary:        Interactive process picker and live exec() tracer (extrace front-end)

License:        0BSD AND BSL-1.0
URL:            https://github.com/binRick/procsentry
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make

%description
procsentry pairs a type-to-search process picker (a subtree-aware
"ps --forest" tree) with a live exec() tracer: select one or more processes
and watch a colour-tagged tree of every program their subtrees run, captured
via extrace. The trace pane needs root and the kernel proc connector; the
process picker works anywhere ps does.

This package bundles the termpaint terminal library (Boost Software License
1.0). procsentry is a front-end for extrace, which must be installed
separately for the trace pane to function.

%prep
%setup -q

%build
make

%install
make install DESTDIR=%{buildroot} PREFIX=%{_prefix}

%files
%{_bindir}/procsentry
%{_bindir}/procsentry-gfx
%dir %{_datadir}/doc/procsentry
%{_datadir}/doc/procsentry/README.md
%{_datadir}/doc/procsentry/LICENSE

%changelog
* Mon Jun 15 2026 procsentry authors <noreply@github.com> - 0.1.0-1
- Initial package
