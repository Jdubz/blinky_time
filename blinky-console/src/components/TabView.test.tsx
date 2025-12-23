import { describe, it, expect } from 'vitest';
import { render, screen, fireEvent } from '@testing-library/react';
import { TabView } from './TabView';

describe('TabView', () => {
  const mockTabs = [
    { id: 'inputs' as const, label: 'Inputs', content: <div>Inputs Content</div> },
    { id: 'generators' as const, label: 'Generators', content: <div>Generators Content</div> },
    { id: 'effects' as const, label: 'Effects', content: <div>Effects Content</div> },
  ];

  describe('rendering', () => {
    it('renders all tab buttons', () => {
      render(<TabView tabs={mockTabs} />);

      expect(screen.getByRole('tab', { name: 'Inputs' })).toBeInTheDocument();
      expect(screen.getByRole('tab', { name: 'Generators' })).toBeInTheDocument();
      expect(screen.getByRole('tab', { name: 'Effects' })).toBeInTheDocument();
    });

    it('renders the default tab content', () => {
      render(<TabView tabs={mockTabs} />);

      expect(screen.getByText('Inputs Content')).toBeInTheDocument();
      expect(screen.queryByText('Generators Content')).not.toBeInTheDocument();
      expect(screen.queryByText('Effects Content')).not.toBeInTheDocument();
    });

    it('renders custom default tab content', () => {
      render(<TabView tabs={mockTabs} defaultTab="generators" />);

      expect(screen.getByText('Generators Content')).toBeInTheDocument();
      expect(screen.queryByText('Inputs Content')).not.toBeInTheDocument();
    });

    it('has tablist role on tab header', () => {
      render(<TabView tabs={mockTabs} />);

      const tablist = screen.getByRole('tablist');
      expect(tablist).toBeInTheDocument();
      expect(tablist).toHaveAttribute('aria-label', 'Settings categories');
    });

    it('has tabpanel role on content area', () => {
      render(<TabView tabs={mockTabs} />);

      const tabpanel = screen.getByRole('tabpanel');
      expect(tabpanel).toBeInTheDocument();
    });
  });

  describe('tab switching', () => {
    it('switches tabs on click', () => {
      render(<TabView tabs={mockTabs} />);

      const generatorsTab = screen.getByRole('tab', { name: 'Generators' });
      fireEvent.click(generatorsTab);

      expect(screen.getByText('Generators Content')).toBeInTheDocument();
      expect(screen.queryByText('Inputs Content')).not.toBeInTheDocument();
    });

    it('updates active state when switching tabs', () => {
      render(<TabView tabs={mockTabs} />);

      const inputsTab = screen.getByRole('tab', { name: 'Inputs' });
      const generatorsTab = screen.getByRole('tab', { name: 'Generators' });

      expect(inputsTab).toHaveAttribute('aria-selected', 'true');
      expect(generatorsTab).toHaveAttribute('aria-selected', 'false');

      fireEvent.click(generatorsTab);

      expect(inputsTab).toHaveAttribute('aria-selected', 'false');
      expect(generatorsTab).toHaveAttribute('aria-selected', 'true');
    });

    it('applies active class to selected tab', () => {
      render(<TabView tabs={mockTabs} />);

      const inputsTab = screen.getByRole('tab', { name: 'Inputs' });
      const generatorsTab = screen.getByRole('tab', { name: 'Generators' });

      expect(inputsTab).toHaveClass('active');
      expect(generatorsTab).not.toHaveClass('active');

      fireEvent.click(generatorsTab);

      expect(inputsTab).not.toHaveClass('active');
      expect(generatorsTab).toHaveClass('active');
    });
  });

  describe('ARIA attributes', () => {
    it('sets correct aria-controls on tabs', () => {
      render(<TabView tabs={mockTabs} />);

      const inputsTab = screen.getByRole('tab', { name: 'Inputs' });
      const generatorsTab = screen.getByRole('tab', { name: 'Generators' });

      expect(inputsTab).toHaveAttribute('aria-controls', 'tabpanel-inputs');
      expect(generatorsTab).toHaveAttribute('aria-controls', 'tabpanel-generators');
    });

    it('sets correct id on tabs', () => {
      render(<TabView tabs={mockTabs} />);

      const inputsTab = screen.getByRole('tab', { name: 'Inputs' });
      expect(inputsTab).toHaveAttribute('id', 'tab-inputs');
    });

    it('sets correct aria-labelledby on tabpanel', () => {
      render(<TabView tabs={mockTabs} />);

      const tabpanel = screen.getByRole('tabpanel');
      expect(tabpanel).toHaveAttribute('aria-labelledby', 'tab-inputs');
    });

    it('updates aria-labelledby when switching tabs', () => {
      render(<TabView tabs={mockTabs} />);

      const generatorsTab = screen.getByRole('tab', { name: 'Generators' });
      fireEvent.click(generatorsTab);

      const tabpanel = screen.getByRole('tabpanel');
      expect(tabpanel).toHaveAttribute('aria-labelledby', 'tab-generators');
    });
  });

  describe('keyboard navigation', () => {
    it('navigates to next tab with ArrowRight', () => {
      render(<TabView tabs={mockTabs} />);

      const inputsTab = screen.getByRole('tab', { name: 'Inputs' });
      fireEvent.keyDown(inputsTab, { key: 'ArrowRight' });

      expect(screen.getByText('Generators Content')).toBeInTheDocument();
      expect(screen.queryByText('Inputs Content')).not.toBeInTheDocument();
    });

    it('navigates to previous tab with ArrowLeft', () => {
      render(<TabView tabs={mockTabs} defaultTab="generators" />);

      const generatorsTab = screen.getByRole('tab', { name: 'Generators' });
      fireEvent.keyDown(generatorsTab, { key: 'ArrowLeft' });

      expect(screen.getByText('Inputs Content')).toBeInTheDocument();
      expect(screen.queryByText('Generators Content')).not.toBeInTheDocument();
    });

    it('wraps to last tab with ArrowLeft on first tab', () => {
      render(<TabView tabs={mockTabs} />);

      const inputsTab = screen.getByRole('tab', { name: 'Inputs' });
      fireEvent.keyDown(inputsTab, { key: 'ArrowLeft' });

      expect(screen.getByText('Effects Content')).toBeInTheDocument();
    });

    it('wraps to first tab with ArrowRight on last tab', () => {
      render(<TabView tabs={mockTabs} defaultTab="effects" />);

      const effectsTab = screen.getByRole('tab', { name: 'Effects' });
      fireEvent.keyDown(effectsTab, { key: 'ArrowRight' });

      expect(screen.getByText('Inputs Content')).toBeInTheDocument();
    });

    it('navigates to first tab with Home key', () => {
      render(<TabView tabs={mockTabs} defaultTab="effects" />);

      const effectsTab = screen.getByRole('tab', { name: 'Effects' });
      fireEvent.keyDown(effectsTab, { key: 'Home' });

      expect(screen.getByText('Inputs Content')).toBeInTheDocument();
    });

    it('navigates to last tab with End key', () => {
      render(<TabView tabs={mockTabs} />);

      const inputsTab = screen.getByRole('tab', { name: 'Inputs' });
      fireEvent.keyDown(inputsTab, { key: 'End' });

      expect(screen.getByText('Effects Content')).toBeInTheDocument();
    });

    it('does not change tab on unrelated key press', () => {
      render(<TabView tabs={mockTabs} />);

      const inputsTab = screen.getByRole('tab', { name: 'Inputs' });
      fireEvent.keyDown(inputsTab, { key: 'Enter' });

      expect(screen.getByText('Inputs Content')).toBeInTheDocument();
    });
  });

  describe('tab focus management', () => {
    it('sets tabIndex 0 on active tab', () => {
      render(<TabView tabs={mockTabs} />);

      const inputsTab = screen.getByRole('tab', { name: 'Inputs' });
      const generatorsTab = screen.getByRole('tab', { name: 'Generators' });

      expect(inputsTab).toHaveAttribute('tabIndex', '0');
      expect(generatorsTab).toHaveAttribute('tabIndex', '-1');
    });

    it('updates tabIndex when switching tabs', () => {
      render(<TabView tabs={mockTabs} />);

      const inputsTab = screen.getByRole('tab', { name: 'Inputs' });
      const generatorsTab = screen.getByRole('tab', { name: 'Generators' });

      fireEvent.click(generatorsTab);

      expect(inputsTab).toHaveAttribute('tabIndex', '-1');
      expect(generatorsTab).toHaveAttribute('tabIndex', '0');
    });
  });
});
